// The phone view's HTTP server. See include/web_server.h for why it has no
// thread of its own.

#include "web_server.h"

#include <switch.h>
#include <SDL2/SDL.h>

#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

bool wouldBlock() {
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS;
}

bool setNonBlocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

const char* statusText(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 431: return "Request Header Fields Too Large";
        default:  return "Internal Server Error";
    }
}

// Header lookup over the raw head, case-insensitive on the name.
std::string headerValue(const std::string& head, const char* name) {
    const size_t nameLen = std::strlen(name);
    size_t pos = head.find("\r\n");
    while (pos != std::string::npos) {
        const size_t start = pos + 2;
        const size_t end = head.find("\r\n", start);
        const size_t lineEnd = end == std::string::npos ? head.size() : end;
        if (lineEnd - start > nameLen && head[start + nameLen] == ':'
            && strncasecmp(head.c_str() + start, name, nameLen) == 0) {
            size_t v = start + nameLen + 1;
            while (v < lineEnd && head[v] == ' ') v++;
            return head.substr(v, lineEnd - v);
        }
        pos = end;
    }
    return std::string();
}

} // anonymous namespace

// --- Request ------------------------------------------------------------------

std::string WebServer::Request::param(const char* name) const {
    const size_t nameLen = std::strlen(name);
    size_t pos = 0;
    while (pos <= query.size()) {
        const size_t end = query.find('&', pos);
        const size_t stop = end == std::string::npos ? query.size() : end;
        if (stop - pos > nameLen && query[pos + nameLen] == '='
            && query.compare(pos, nameLen, name) == 0)
            return query.substr(pos + nameLen + 1, stop - pos - nameLen - 1);
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return std::string();
}

int WebServer::Request::intParam(const char* name, int fallback) const {
    const std::string v = param(name);
    if (v.empty() || v.size() > 9) return fallback;
    for (char c : v)
        if (c < '0' || c > '9') return fallback;
    return std::atoi(v.c_str());
}

// --- Lifecycle ----------------------------------------------------------------

bool WebServer::start(uint16_t port, Handler handler) {
    stop();
    error_.clear();
    handler_ = std::move(handler);
    requests_ = 0;
    served_ = 0;

    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        error_ = "socket: " + std::string(std::strerror(errno));
        return false;
    }
    const int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    // The usual port first; a few more in case something else holds it.
    bool bound = false;
    for (uint16_t p = port; p < port + 10; p++) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(p);
        if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            port_ = p;
            bound = true;
            break;
        }
    }
    if (!bound) {
        error_ = "bind: " + std::string(std::strerror(errno));
        close(fd);
        return false;
    }
    if (listen(fd, 8) != 0 || !setNonBlocking(fd)) {
        error_ = "listen: " + std::string(std::strerror(errno));
        close(fd);
        return false;
    }
    listenFd_ = fd;
    return true;
}

void WebServer::stop() {
    for (auto& c : conns_) closeConn(c);
    conns_.clear();
    if (listenFd_ >= 0) {
        close(listenFd_);
        listenFd_ = -1;
    }
}

void WebServer::closeConn(Conn& c) {
    if (c.fd >= 0) {
        close(c.fd);
        c.fd = -1;
    }
}

// --- The loop -----------------------------------------------------------------

bool WebServer::tick(uint32_t budgetMs) {
    if (listenFd_ < 0) return false;
    bool any = false;
    const uint32_t start = SDL_GetTicks();

    // Round after round while something moves: a sprite is one round, and
    // a page of thirty of them should not take thirty frames.
    for (;;) {
        bool progress = acceptPending();
        for (auto& c : conns_) {
            if (c.fd < 0) continue;
            if (c.sent < c.out.size()) {
                progress |= sendTo(c);
                continue;
            }
            progress |= readFrom(c);
            if (c.fd >= 0) progress |= answerReady(c);
            if (c.fd >= 0 && c.sent < c.out.size()) progress |= sendTo(c);
        }

        const uint32_t now = SDL_GetTicks();
        for (auto& c : conns_)
            if (c.fd >= 0 && c.out.empty() && now - c.lastActive > IDLE_MS)
                closeConn(c);
        const size_t before = conns_.size();
        conns_.erase(std::remove_if(conns_.begin(), conns_.end(),
                                    [](const Conn& c) { return c.fd < 0; }),
                     conns_.end());
        progress |= conns_.size() != before;

        any |= progress;
        if (!progress || now - start >= budgetMs) break;
    }
    return any;
}

bool WebServer::acceptPending() {
    bool any = false;
    for (;;) {
        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        const int fd = accept(listenFd_, reinterpret_cast<sockaddr*>(&addr), &len);
        if (fd < 0) break;   // EAGAIN: nobody else waiting
        any = true;
        if (conns_.size() >= MAX_CONNS || !setNonBlocking(fd)) {
            close(fd);
            continue;
        }
        Conn c;
        c.fd = fd;
        c.lastActive = SDL_GetTicks();
        conns_.push_back(std::move(c));
    }
    return any;
}

bool WebServer::readFrom(Conn& c) {
    char buf[2048];
    bool any = false;
    for (;;) {
        const ssize_t n = recv(c.fd, buf, sizeof(buf), 0);
        if (n > 0) {
            c.in.append(buf, static_cast<size_t>(n));
            c.lastActive = SDL_GetTicks();
            any = true;
            if (c.in.size() > MAX_REQUEST) break;
            continue;
        }
        if (n == 0 || !wouldBlock())
            closeConn(c);   // closed by the phone, or broken
        break;
    }
    return any || c.fd < 0;
}

bool WebServer::sendTo(Conn& c) {
    bool any = false;
    while (c.sent < c.out.size()) {
        const ssize_t n = send(c.fd, c.out.data() + c.sent, c.out.size() - c.sent, 0);
        if (n > 0) {
            c.sent += static_cast<size_t>(n);
            c.lastActive = SDL_GetTicks();
            any = true;
            continue;
        }
        if (n < 0 && wouldBlock()) return any;
        closeConn(c);
        return true;
    }
    // All of it sent: ready for the next request on the same connection.
    c.out.clear();
    c.sent = 0;
    if (c.closeAfter) closeConn(c);
    return true;
}

void WebServer::respond(Conn& c, const std::string& head, bool wantsClose) {
    c.out = head;
    c.sent = 0;
    c.closeAfter = wantsClose;
}

bool WebServer::answerReady(Conn& c) {
    if (!c.out.empty()) return false;   // one answer at a time
    const size_t headEnd = c.in.find("\r\n\r\n");
    if (headEnd == std::string::npos) {
        if (c.in.size() > MAX_REQUEST) {
            respond(c, "HTTP/1.1 431 Request Header Fields Too Large\r\n"
                       "Content-Length: 0\r\nConnection: close\r\n\r\n", true);
            return true;
        }
        return false;
    }
    const std::string head = c.in.substr(0, headEnd);
    c.in.erase(0, headEnd + 4);   // GET has no body; a pipelined one stays
    requests_++;

    // "GET /path?query HTTP/1.1"
    const size_t sp1 = head.find(' ');
    const size_t sp2 = sp1 == std::string::npos ? std::string::npos : head.find(' ', sp1 + 1);
    const size_t lineEnd = head.find("\r\n");
    if (sp1 == std::string::npos || sp2 == std::string::npos
        || (lineEnd != std::string::npos && sp2 > lineEnd)) {
        respond(c, "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", true);
        return true;
    }
    const std::string method = head.substr(0, sp1);
    const std::string target = head.substr(sp1 + 1, sp2 - sp1 - 1);
    const bool http10 = head.compare(sp2 + 1, 8, "HTTP/1.0") == 0;
    std::string connHdr = headerValue(head, "Connection");
    for (auto& ch : connHdr) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    const bool wantsClose = http10 ? connHdr != "keep-alive" : connHdr == "close";

    if (method != "GET" && method != "HEAD") {
        respond(c, "HTTP/1.1 405 Method Not Allowed\r\nAllow: GET\r\n"
                   "Content-Length: 0\r\nConnection: close\r\n\r\n", true);
        return true;
    }

    Request req;
    const size_t q = target.find('?');
    req.path = target.substr(0, q);
    if (q != std::string::npos) req.query = target.substr(q + 1);

    served_++;
    Response res = handler_ ? handler_(req) : Response{404, "text/plain", "", false};

    char line[256];
    std::snprintf(line, sizeof(line), "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n",
                  res.status, statusText(res.status), res.type.c_str(), res.body.size());
    std::string out = line;
    out += res.cacheable ? "Cache-Control: private, max-age=86400\r\n" : "Cache-Control: no-store\r\n";
    out += "X-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\n";
    out += wantsClose ? "Connection: close\r\n\r\n" : "Connection: keep-alive\r\n\r\n";
    if (method == "GET") out += res.body;
    respond(c, out, wantsClose);
    return true;
}
