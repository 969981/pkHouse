#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// A very small HTTP server for the phone view: GET only, on the local network,
// driven from the caller's loop rather than from a thread of its own.
//
// Nothing here blocks. tick() accepts what is waiting, reads what has arrived,
// answers the requests that are complete and sends as much of each answer as
// the socket takes, then returns. Called from the UI loop, the handler runs on
// the UI thread and can read the banks and the save as they are in memory,
// with no lock and no copy - which is the reason it is built this way.
//
// There is no key: the page is read-only, and it only exists while the
// screen that started it is open.
class WebServer {
public:
    struct Request {
        std::string path;    // "/api/box", without the query
        std::string query;   // "src=3&b=2", without the '?'
        // One value of the query, "" when it is not there. Not decoded: the
        // routes only ever take numbers.
        std::string param(const char* name) const;
        int intParam(const char* name, int fallback) const;
    };
    struct Response {
        int status = 200;
        std::string type = "application/json";
        std::string body;
        bool cacheable = false;   // sprites: the phone keeps them
    };
    using Handler = std::function<Response(const Request&)>;

    ~WebServer() { stop(); }

    // Listens on the first free port from `port` on. False, with error() set,
    // when no socket could be opened.
    bool start(uint16_t port, Handler handler);
    void stop();
    bool running() const { return listenFd_ >= 0; }

    // Accepts, reads, answers and sends what it can, for at most `budgetMs`.
    // True when anything happened, so the caller knows to redraw its status.
    bool tick(uint32_t budgetMs = 8);

    uint16_t port() const { return port_; }
    const std::string& error() const { return error_; }

    // For the status line: connections open now, and requests answered.
    int connections() const { return static_cast<int>(conns_.size()); }
    int requests() const { return requests_; }
    // Requests the page made and got an answer to: a phone has opened it.
    int served() const { return served_; }

private:
    struct Conn {
        int fd = -1;
        std::string in;          // what has arrived and is not yet answered
        std::string out;         // the answer being sent
        size_t sent = 0;
        bool closeAfter = false; // the client asked to close, or we did
        uint32_t lastActive = 0;
    };

    // Requests are a line and some headers: anything longer is not a browser.
    static constexpr size_t MAX_REQUEST = 8 * 1024;
    // Browsers open about six connections to fetch a page's images.
    static constexpr size_t MAX_CONNS = 12;
    static constexpr uint32_t IDLE_MS = 15000;

    int listenFd_ = -1;
    uint16_t port_ = 0;
    Handler handler_;
    std::string error_;
    std::vector<Conn> conns_;
    int requests_ = 0;
    int served_ = 0;

    bool acceptPending();
    bool readFrom(Conn& c);
    bool answerReady(Conn& c);
    bool sendTo(Conn& c);
    void respond(Conn& c, const std::string& head, bool wantsClose);
    static void closeConn(Conn& c);
};
