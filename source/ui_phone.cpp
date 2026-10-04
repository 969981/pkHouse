// Phone view: the collection on a phone or PC browser, read-only.
//
// Opened on demand only: X on the profile and game screens (banks alone,
// no game needed), or the + menu in the box view. While its screen is up, pkHouse
// serves a small web page on the local network (romfs:/web/index.html) and the
// data it asks for: the save that is open, the banks that are open as they are
// in memory - unsaved changes included - and every other bank of every game as
// it is on the SD card. Closing the screen stops the server; nothing is
// listening the rest of the time.
//
// There is no key in the address: the page is read-only, and it stops with
// the screen.

#define JSON_NOEXCEPTION
#include "json.hpp"

#include "ui.h"
#include "i18n.h"
#include "gts.h"
#include "bank_manager.h"
#include "species_converter.h"
#include "met_info.h"
#include "form_names.h"
#include "species_types.h"
#include "move_types.h"
#include "theme.h"
#include "qrcodegen.h"

#include <switch.h>

#include <algorithm>
#include <cstdio>
#include <memory>

using nlohmann::json;

// One thing the page can show: the save, or one bank.
struct PhoneSource {
    bool        isSave = false;
    GameType    game = GameType::ZA;
    std::string name;    // the bank's name; empty for the save
    std::string path;    // the bank's file; empty for the save
    int         count = 0;
};

struct PhoneSession {
    std::vector<PhoneSource> sources;
    // The banks open in the box view, read from memory rather than the SD
    // card; empty when the phone view was opened before choosing a game.
    std::string activePath;
    std::string leftPath;
    // Banks read off the SD card for the page, most recent last. A few are
    // kept so the summary of a Pokemon does not read its bank again, and only
    // a few: a bank is several hundred KB in memory.
    std::vector<std::pair<int, std::unique_ptr<Bank>>> loaded;
    static constexpr size_t KEEP_LOADED = 3;
};

namespace {

constexpr uint16_t PHONE_PORT = 8080;

// The page's QR code: an address, so a small version is plenty.
constexpr int PH_QR_MAX_VERSION = 10;
constexpr int PH_QR_BUF = qrcodegen_BUFFER_LEN_FOR_VERSION(PH_QR_MAX_VERSION);

// Never let JSON_NOEXCEPTION turn a nickname with a broken byte into an abort:
// invalid UTF-8 is replaced rather than thrown about.
std::string dumpJson(const json& j) {
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

WebServer::Response jsonResponse(const json& j) {
    WebServer::Response r;
    r.body = dumpJson(j);
    return r;
}

WebServer::Response notFound() {
    WebServer::Response r;
    r.status = 404;
    r.type = "application/json";
    r.body = "{\"error\":\"not found\"}";
    return r;
}

bool readWholeFile(const std::string& path, std::string& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    char buf[8192];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
        out.append(buf, n);
    std::fclose(f);
    return true;
}

// "/sprite/25-0.png" -> 25, 0. Digits only: nothing from the request ever
// reaches a file name except as a number.
bool parseSpritePath(const std::string& rest, int& species, int& form) {
    int a = 0, b = 0, i = 0;
    const int n = static_cast<int>(rest.size());
    if (i >= n || rest[i] < '0' || rest[i] > '9') return false;
    while (i < n && rest[i] >= '0' && rest[i] <= '9' && a < 100000) a = a * 10 + (rest[i++] - '0');
    if (i >= n || rest[i] != '-') return false;
    i++;
    if (i >= n || rest[i] < '0' || rest[i] > '9') return false;
    while (i < n && rest[i] >= '0' && rest[i] <= '9' && b < 1000) b = b * 10 + (rest[i++] - '0');
    if (rest.compare(i, std::string::npos, ".png") != 0) return false;
    species = a;
    form = b;
    return species <= 2000 && form <= 255;
}

// "/type/12.png" -> 12. Digits only, as for the sprites.
bool parseNumberPng(const std::string& rest, int& n) {
    n = 0;
    size_t i = 0;
    if (rest.empty() || rest[0] < '0' || rest[0] > '9') return false;
    while (i < rest.size() && rest[i] >= '0' && rest[i] <= '9' && n < 10000) n = n * 10 + (rest[i++] - '0');
    return rest.compare(i, std::string::npos, ".png") == 0;
}

// "/ribbon/ribbonchampiongalar.png" -> "ribbonchampiongalar". Ribbon files are
// lower-case letters and digits; anything else is refused before it can
// become part of a path.
bool parseNamePng(const std::string& rest, std::string& name) {
    const size_t dot = rest.find(".png");
    if (dot == std::string::npos || dot == 0 || dot > 48 || dot + 4 != rest.size()) return false;
    for (size_t i = 0; i < dot; i++) {
        const char c = rest[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    name = rest.substr(0, dot);
    return true;
}

// "Nature: " -> "Nature", as the summary does with the same strings.
std::string stripPrefix(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == ':')) s.pop_back();
    const std::string fw = "\xef\xbc\x9a";   // full-width colon, CJK strings
    if (s.size() >= fw.size() && s.compare(s.size() - fw.size(), fw.size(), fw) == 0)
        s.erase(s.size() - fw.size());
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

std::string cssColor(SDL_Color c) {
    char buf[40];
    if (c.a == 255) std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", c.r, c.g, c.b);
    else std::snprintf(buf, sizeof(buf), "rgba(%u,%u,%u,%.3f)", c.r, c.g, c.b, c.a / 255.0);
    return buf;
}

// The theme in use, as CSS custom properties: the page wears the app's colours.
json themeJson(const Theme& t) {
    json j;
    const std::pair<const char*, SDL_Color> colours[] = {
        {"bg", t.bg}, {"panel", t.panelBg}, {"border", t.panelBorder}, {"divider", t.divider},
        {"text", t.text}, {"dim", t.textDim}, {"muted", t.textMuted},
        {"accent", t.accent}, {"save", t.accentSave}, {"bank", t.accentBank},
        {"slot", t.slotFull}, {"slotEgg", t.slotEgg}, {"slotEmpty", t.slotEmpty},
        {"cellBorder", t.cellBorder}, {"cellEmptyBorder", t.cellEmptyBorder},
        {"match", t.searchMatch}, {"badge", t.badgeBg}, {"button", t.buttonBg},
        {"buttonBorder", t.buttonBorder}, {"keyCap", t.keyCap}, {"keyCapText", t.keyCapText},
        {"male", t.genderMale}, {"female", t.genderFemale}, {"red", t.red}, {"ok", t.statusOk},
    };
    for (const auto& [name, c] : colours) j[name] = cssColor(c);
    return j;
}

// The summary's own words, in the app's language, so the page says what the
// Switch says.
json labelsJson() {
    json l;
    l["nature"]   = stripPrefix(i18n::get(StrKey::NaturePrefix));
    l["ability"]  = stripPrefix(i18n::get(StrKey::AbilityPrefix));
    l["item"]     = stripPrefix(i18n::get(StrKey::HeldItemPrefix));
    l["ot"]       = stripPrefix(i18n::get(StrKey::OTPrefix));
    l["ht"]       = stripPrefix(i18n::get(StrKey::HTPrefix));
    l["tera"]     = i18n::get(StrKey::SumTera);
    l["ivs"]      = i18n::get(StrKey::IVs);
    l["evs"]      = i18n::get(StrKey::EVs);
    l["origin"]   = i18n::get(StrKey::SumOrigin);
    l["met"]      = i18n::get(StrKey::SumMet);
    l["location"] = i18n::get(StrKey::SumLocation);
    l["lang"]     = i18n::get(StrKey::SumLang);
    l["none"]     = i18n::get(StrKey::NoneItem);
    l["lv"]       = i18n::get(StrKey::LvPrefix);
    l["perfect"]  = i18n::get(StrKey::InfoPerfectIvs);
    l["ribbonOne"]  = i18n::get(StrKey::SumRibbonOne);
    l["ribbonMany"] = i18n::get(StrKey::SumRibbonMany);
    l["markOne"]    = i18n::get(StrKey::SumMarkOne);
    l["markMany"]   = i18n::get(StrKey::SumMarkMany);
    l["noRibbons"]  = i18n::get(StrKey::SumNoRibbons);
    l["ribbons"]    = i18n::get(StrKey::RibbonsMarks);
    l["save"]       = i18n::get(StrKey::TabSave);
    l["bank"]       = i18n::get(StrKey::TabBank);
    // Showdown order, as the EV bars and the API's ivs / evs arrays.
    // The page's own words.
    l["search"]  = i18n::get(StrKey::WebSearch);
    l["box"]     = i18n::get(StrKey::WebBox);
    l["of"]      = i18n::get(StrKey::WebOf);
    l["empty"]   = i18n::get(StrKey::WebEmpty);
    l["noMatch"] = i18n::get(StrKey::WebNone);
    l["matches"] = i18n::get(StrKey::WebMatches);
    l["loading"] = i18n::get(StrKey::WebLoading);
    l["lost"]    = i18n::get(StrKey::WebLost);
    l["shiny"]   = i18n::get(StrKey::LegendShiny);
    l["alpha"]   = i18n::get(StrKey::LegendAlpha);
    l["gmax"]    = i18n::get(StrKey::WebGmax);
    l["egg"]     = i18n::get(StrKey::Egg);
    l["stats"] = {i18n::get(StrKey::StatHP), i18n::get(StrKey::StatAtk), i18n::get(StrKey::StatDef),
                  i18n::get(StrKey::StatSpA), i18n::get(StrKey::StatSpD), i18n::get(StrKey::StatSpe)};
    return l;
}

// The QR code, pure black on white with whole-pixel modules - the same way
// the card draws its own (ui_card.cpp).
void drawQrCode(SDL_Renderer* r, const uint8_t* qrcode, int bx, int by, int box) {
    const int size = qrcodegen_getSize(qrcode);
    if (size <= 0) return;
    int scale = box / (size + 8);
    if (scale < 1) scale = 1;
    const int drawn = size * scale;
    const int ox = bx + (box - drawn) / 2;
    const int oy = by + (box - drawn) / 2;
    std::vector<SDL_Rect> rects;
    for (int y = 0; y < size; y++) {
        int runStart = -1;
        for (int x = 0; x <= size; x++) {
            const bool dark = x < size && qrcodegen_getModule(qrcode, x, y);
            if (dark && runStart < 0) {
                runStart = x;
            } else if (!dark && runStart >= 0) {
                rects.push_back({ox + runStart * scale, oy + y * scale,
                                 (x - runStart) * scale, scale});
                runStart = -1;
            }
        }
    }
    if (rects.empty()) return;
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderFillRects(r, rects.data(), static_cast<int>(rects.size()));
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
}

// The console's address on the network, or "" when it has none.
std::string consoleAddress() {
    u32 ip = 0;
    if (R_FAILED(nifmInitialize(NifmServiceType_User))) return std::string();
    const Result rc = nifmGetCurrentIpAddress(&ip);
    nifmExit();
    if (R_FAILED(rc) || ip == 0) return std::string();
    // Network order: the first byte in memory is the first number.
    const uint8_t a = ip & 0xFF, b = (ip >> 8) & 0xFF, c = (ip >> 16) & 0xFF, d = ip >> 24;
    if (a == 127) return std::string();
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", a, b, c, d);
    return buf;
}

// What the grid draws for one slot.
json slotJson(const Pokemon& pkm) {
    const bool egg = pkm.isEgg();
    json s;
    s["i"] = pkm.species();
    s["f"] = pkm.form();
    const std::string name = pkm.displayName();
    s["n"] = name;
    // The species as well when a nickname hides it, so searching "Eevee"
    // finds the Eevee called Bob.
    const std::string& species = SpeciesName::get(pkm.species());
    if (!egg && species != name) s["sp"] = species;
    s["l"] = pkm.level();
    s["g"] = pkm.gender();
    if (egg) s["e"] = 1;
    if (pkm.isShiny()) s["s"] = 1;
    if (pkm.isAlpha()) s["a"] = 1;
    if (pkm.canGigantamax()) s["x"] = 1;
    return s;
}

// Everything the summary shows, in the shape the page draws it.
json pokemonJson(const Pokemon& pkm) {
    const uint16_t species = pkm.species();
    const GameType game = pkm.gameType_;
    const bool egg = pkm.isEgg();
    json p;
    p["species"] = species;
    p["speciesName"] = SpeciesName::get(species);
    p["form"] = pkm.form();
    if (!egg) {
        const std::string nick = pkm.nickname();
        if (pkm.isNicknamed() && !nick.empty() && nick != SpeciesName::get(species))
            p["nickname"] = nick;
        if (const char* form = getFormName(species, pkm.form()))
            p["formName"] = form;
        const SpeciesTypes::Pair types = SpeciesTypes::get(game, species, pkm.form());
        json t = json::array();
        if (types.t1 < 18) t.push_back(json{{"id", types.t1}, {"name", TypeName::get(types.t1)}});
        if (types.t2 < 18 && types.t2 != types.t1)
            t.push_back(json{{"id", types.t2}, {"name", TypeName::get(types.t2)}});
        p["types"] = t;
    }
    p["level"] = pkm.level();
    p["gender"] = pkm.gender();
    p["egg"] = egg;
    p["shiny"] = pkm.isShiny();
    p["alpha"] = pkm.isAlpha();
    p["gmax"] = pkm.canGigantamax();

    const uint8_t nature = pkm.nature();
    p["nature"] = NatureName::get(nature);
    if (nature / 5 != nature % 5) {
        p["natureUp"] = NatureStat::get(nature / 5);
        p["natureDown"] = NatureStat::get(nature % 5);
    }
    p["ability"] = AbilityName::get(pkm.ability());
    if (pkm.heldItem()) p["item"] = ItemName::get(pkm.heldItem());
    p["ball"] = pkm.ball();
    if (const char* ball = BallName::get(pkm.ball()); ball && ball[0]) p["ballName"] = ball;

    // Scarlet / Violet only; the byte means something else elsewhere.
    const uint8_t tera = pkm.teraType();
    if (tera <= 17)                         p["tera"] = json{{"id", tera}, {"name", TypeName::get(tera)}};
    else if (tera == Pokemon::TERA_STELLAR) p["tera"] = json{{"id", 99}, {"name", "STELLAR"}};

    json moves = json::array();
    for (uint16_t m : {pkm.move1(), pkm.move2(), pkm.move3(), pkm.move4()}) {
        if (!m) { moves.push_back(nullptr); continue; }
        json mv;
        mv["name"] = MoveName::get(m);
        const uint8_t type = getMoveType(m, game);
        if (type < 18) mv["type"] = json{{"id", type}, {"name", TypeName::get(type)}};
        moves.push_back(mv);
    }
    p["moves"] = moves;

    // Showdown order: HP, Atk, Def, SpA, SpD, Spe.
    p["ivs"] = {pkm.ivHp(), pkm.ivAtk(), pkm.ivDef(), pkm.ivSpA(), pkm.ivSpD(), pkm.ivSpe()};
    p["evs"] = {pkm.evHp(), pkm.evAtk(), pkm.evDef(), pkm.evSpA(), pkm.evSpD(), pkm.evSpe()};

    p["ot"] = pkm.otName();
    p["tid"] = pkm.displayTid();
    p["sid"] = pkm.displaySid();
    if (pkm.hasHandlingTrainer()) p["ht"] = pkm.htName();
    if (const char* v = VersionName::get(pkm.originVersion()); v && v[0]) p["origin"] = v;
    if (pkm.metYear()) {
        char date[16];
        std::snprintf(date, sizeof(date), "%04u-%02u-%02u", static_cast<unsigned>(pkm.metYear()),
                      static_cast<unsigned>(pkm.metMonth()), static_cast<unsigned>(pkm.metDay()));
        p["metOn"] = date;
    }
    const std::string loc = LocationName::get(game, pkm.metLocation());
    if (!loc.empty()) p["metAt"] = loc;
    p["lang"] = LanguageTag::get(pkm.language());

    // The quiet line under everything: PID, EC, raw IDs, TSV.
    char tech[128];
    std::snprintf(tech, sizeof(tech), "PID %08X    EC %08X    ID %05u / %05u    TSV %04u",
                  static_cast<unsigned>(pkm.pid()), static_cast<unsigned>(pkm.encryptionConstant()),
                  static_cast<unsigned>(pkm.tid()), static_cast<unsigned>(pkm.sid()),
                  static_cast<unsigned>((pkm.tid() ^ pkm.sid()) >> 4));
    p["tech"] = tech;

    json ribbons = json::array();
    for (const auto& r : pkm.getRibbonsAndMarks())
        ribbons.push_back(json{{"name", r.name}, {"file", r.filename}, {"mark", r.isMark}});
    p["ribbons"] = ribbons;
    return p;
}

} // anonymous namespace

// --- Routes ---------------------------------------------------------------------

WebServer::Response UI::phoneRoute(PhoneSession& session, const WebServer::Request& req) {
    const std::string& path = req.path;

    if (path == "/" || path == "/index.html") {
        WebServer::Response r;
        r.type = "text/html; charset=utf-8";
        if (!readWholeFile("romfs:/web/index.html", r.body)) return notFound();
        return r;
    }

    // Sprites, straight from romfs: the form's own when there is one, the
    // species' otherwise, and the plain one when there is no shiny one.
    const bool shinyPath = path.rfind("/shiny/", 0) == 0;
    if (shinyPath || path.rfind("/sprite/", 0) == 0) {
        int species = 0, form = 0;
        if (!parseSpritePath(path.substr(shinyPath ? 7 : 8), species, form)) return notFound();
        WebServer::Response r;
        r.type = "image/png";
        r.cacheable = true;
        char file[96];
        for (const char* dir : {shinyPath ? "sprites_shiny" : "sprites", "sprites"}) {
            if (form) {
                std::snprintf(file, sizeof(file), "romfs:/%s/%03d-%d.png", dir, species, form);
                if (readWholeFile(file, r.body)) return r;
            }
            std::snprintf(file, sizeof(file), "romfs:/%s/%03d.png", dir, species);
            if (readWholeFile(file, r.body)) return r;
        }
        return notFound();
    }

    // The summary's pictures, from the same romfs folders the app draws them
    // from. Numbers for the numbered ones, a checked name for ribbons, and a
    // fixed list for the status icons.
    auto png = [](const std::string& file) {
        WebServer::Response r;
        r.type = "image/png";
        r.cacheable = true;
        if (!readWholeFile(file, r.body)) return notFound();
        return r;
    };
    if (path.rfind("/type/", 0) == 0) {
        int n = 0;
        if (!parseNumberPng(path.substr(6), n) || (n > 17 && n != 99)) return notFound();
        char file[64];
        std::snprintf(file, sizeof(file), "romfs:/types/type_icon_s_%02d.png", n);
        return png(file);
    }
    if (path.rfind("/ball/", 0) == 0) {
        int n = 0;
        if (!parseNumberPng(path.substr(6), n) || n > 255) return notFound();
        return png("romfs:/balls/_ball" + std::to_string(n) + ".png");
    }
    if (path.rfind("/ribbon/", 0) == 0) {
        std::string name;
        if (!parseNamePng(path.substr(8), name)) return notFound();
        return png("romfs:/ribbons/" + name + ".png");
    }
    if (path.rfind("/icon/", 0) == 0) {
        std::string name;
        if (!parseNamePng(path.substr(6), name)) return notFound();
        for (const char* allowed : {"shiny", "alpha", "shiny_alpha", "dynamax", "house"})
            if (name == allowed) return png("romfs:/icons/" + name + ".png");
        return notFound();
    }

    if (path == "/api/sources") {
        json list = json::array();
        for (size_t i = 0; i < session.sources.size(); i++) {
            const auto& s = session.sources[i];
            json e;
            e["id"] = i;
            e["kind"] = s.isSave ? "save" : "bank";
            e["group"] = bankGroupNameOf(s.game);
            e["name"] = s.isSave ? std::string(gameDisplayNameOf(s.game)) : s.name;
            e["count"] = s.count;
            list.push_back(e);
        }
        json j;
        j["app"] = "pkHouse";
        j["version"] = APP_VERSION;
        j["lang"] = i18n::currentLang();
        j["theme"] = themeJson(T());
        j["labels"] = labelsJson();
        j["sources"] = list;
        return jsonResponse(j);
    }

    const bool wantBox = path == "/api/box", wantPkm = path == "/api/pkm";
    if (!wantBox && !wantPkm) return notFound();

    const int id = req.intParam("src", -1);
    if (id < 0 || id >= static_cast<int>(session.sources.size())) return notFound();
    const PhoneSource& src = session.sources[id];

    // Where this source's Pokemon are: the save, an open bank in memory, or a
    // bank read off the SD card for the page.
    const Bank* bank = nullptr;
    if (!src.isSave) {
        if (!session.activePath.empty() && src.path == session.activePath)  bank = &bank_;
        else if (!session.leftPath.empty() && src.path == session.leftPath) bank = &bankLeft_;
        else {
            auto it = std::find_if(session.loaded.begin(), session.loaded.end(),
                                   [&](const auto& p) { return p.first == id; });
            if (it == session.loaded.end()) {
                auto b = std::make_unique<Bank>();
                b->setGameType(src.game);
                if (!b->load(src.path)) return notFound();
                if (session.loaded.size() >= PhoneSession::KEEP_LOADED)
                    session.loaded.erase(session.loaded.begin());
                session.loaded.emplace_back(id, std::move(b));
                it = session.loaded.end() - 1;
            }
            bank = it->second.get();
        }
    }
    const int boxes  = bank ? bank->boxCount()   : save_.boxCount();
    const int perBox = bank ? bank->slotsPerBox() : save_.slotsPerBox();
    auto pokemonAt = [&](int b, int s) {
        return bank ? bank->getSlot(b, s) : save_.getBoxSlot(b, s);
    };

    if (wantPkm) {
        const int b = req.intParam("b", -1), s = req.intParam("s", -1);
        if (b < 0 || b >= boxes || s < 0 || s >= perBox) return notFound();
        const Pokemon pkm = pokemonAt(b, s);
        if (pkm.isEmpty()) return notFound();
        return jsonResponse(pokemonJson(pkm));
    }

    json boxList = json::array();
    for (int b = 0; b < boxes; b++) {
        json slots = json::array();
        for (int s = 0; s < perBox; s++) {
            if (bank && bank->isSlotEmpty(b, s)) { slots.push_back(nullptr); continue; }
            const Pokemon pkm = pokemonAt(b, s);
            if (pkm.isEmpty()) slots.push_back(nullptr);
            else               slots.push_back(slotJson(pkm));
        }
        json box;
        box["name"] = bank ? bank->getBoxName(b) : save_.getBoxName(b);
        box["slots"] = slots;
        boxList.push_back(box);
    }
    json j;
    j["id"] = id;
    j["kind"] = src.isSave ? "save" : "bank";
    j["group"] = bankGroupNameOf(src.game);
    j["name"] = src.isSave ? std::string(gameDisplayNameOf(src.game)) : src.name;
    j["cols"] = isLGPE(src.game) ? 5 : 6;
    // Which filters mean anything here: alphas exist in Legends games only,
    // Gigantamax in Sword / Shield only - as the app's own search offers them.
    j["hasAlpha"] = gameInfo(src.game).hasAlphaForms;
    j["hasGmax"] = isSwSh(src.game);
    j["boxes"] = boxList;
    return jsonResponse(j);
}

// --- The screen -------------------------------------------------------------------

void UI::showPhoneView() {
    showWorking(i18n::get(StrKey::PhonePreparing));

    if (!Gts::startNetwork(basePath_)) {
        showMessageAndWait(i18n::get(StrKey::PhoneFailedTitle),
                           i18n::fmt(StrKey::PhoneFailedBody, Gts::lastError()));
        return;
    }
    const std::string ip = consoleAddress();
    if (ip.empty()) {
        showMessageAndWait(i18n::get(StrKey::PhoneNoNetworkTitle),
                           i18n::get(StrKey::PhoneNoNetworkBody));
        return;
    }

    // What the page can open: the save first, then every bank of every game,
    // grouped the way All banks groups them. Counted now, once; the open
    // banks from memory, so a Pokemon moved a minute ago is counted where it is.
    // From the profile and game screens there is no session: only the box
    // view has a save and banks open. Anywhere else, whatever is still in
    // memory belongs to a game already left, and the page reads the SD card.
    const bool inSession = screen_ == AppScreen::MainView;
    const std::string livePath  = inSession ? activeBankPath_ : std::string();
    const std::string leftPath  = inSession ? leftBankPath_   : std::string();
    PhoneSession session;
    session.activePath = livePath;
    session.leftPath = leftPath;
    if (inSession && !isDualBankMode() && save_.isLoaded()) {
        PhoneSource s;
        s.isSave = true;
        s.game = selectedGame_;
        for (int b = 0; b < save_.boxCount(); b++)
            for (int i = 0; i < save_.slotsPerBox(); i++)
                if (!save_.getBoxSlot(b, i).isEmpty()) s.count++;
        session.sources.push_back(s);
    }
    auto countInMemory = [](const Bank& bank) {
        int n = 0;
        for (int b = 0; b < bank.boxCount(); b++)
            for (int i = 0; i < bank.slotsPerBox(); i++)
                if (!bank.isSlotEmpty(b, i)) n++;
        return n;
    };
    for (GameType family : BankManager::FAMILY_GAMES) {
        const std::string dir = basePath_ + "banks/" + bankFolderNameOf(family) + "/";
        for (const auto& name : BankManager::bankNames(basePath_, family)) {
            PhoneSource s;
            s.game = family;
            s.name = name;
            s.path = dir + name + ".bin";
            if (!livePath.empty() && s.path == livePath)
                s.count = countInMemory(bank_);
            else if (!leftPath.empty() && s.path == leftPath)
                s.count = countInMemory(bankLeft_);
            else
                s.count = Bank::countOccupiedInFile(s.path);
            session.sources.push_back(std::move(s));
        }
    }

    WebServer server;
    if (!server.start(PHONE_PORT,
                      [&](const WebServer::Request& r) { return phoneRoute(session, r); })) {
        showMessageAndWait(i18n::get(StrKey::PhoneFailedTitle),
                           i18n::fmt(StrKey::PhoneFailedBody, server.error()));
        return;
    }
    const std::string url = "http://" + ip + ":" + std::to_string(server.port()) + "/";

    uint8_t qrcode[PH_QR_BUF];
    {
        uint8_t temp[PH_QR_BUF];
        if (!qrcodegen_encodeText(url.c_str(), temp, qrcode, qrcodegen_Ecc_MEDIUM, 1,
                                  PH_QR_MAX_VERSION, qrcodegen_Mask_AUTO, true))
            qrcode[0] = 0;   // no code: the address is still shown as text
    }

    // A phone reading the page is not someone using the Switch: keep the
    // console from dimming and going to sleep, which would drop the network.
    appletSetMediaPlaybackState(true);
    markDirty();
    updateStick(0, 0);

    bool open = true, redraw = true;
    int shownServed = -1;
    uint32_t lastDraw = 0;
    while (open) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) { open = false; break; }
            if (event.type != SDL_CONTROLLERBUTTONDOWN) continue;
            switch (event.cbutton.button) {
                case SDL_CONTROLLER_BUTTON_A:       // Switch B = stop
                case SDL_CONTROLLER_BUTTON_START:   // + closes it too, as it opened it
                    open = false;
                    break;
            }
        }
        if (!open) break;

        const bool busy = server.tick();

        // The status line follows the server, at most four times a second.
        const uint32_t now = SDL_GetTicks();
        if (server.served() != shownServed && now - lastDraw >= 250)
            redraw = true;
        if (redraw) {
            shownServed = server.served();
            drawPhoneView(url, qrcode[0] ? qrcode : nullptr, server);
            SDL_RenderPresent(renderer_);
            lastDraw = now;
            redraw = false;
        }
        // Serving: come back quickly for the rest. Idle: the usual frame.
        SDL_Delay(busy ? 1 : 16);
    }

    server.stop();
    appletSetMediaPlaybackState(false);
    updateStick(0, 0);
    markDirty();
}

void UI::drawPhoneView(const std::string& url, const uint8_t* qrcode, const WebServer& server) {
    drawDialogBackdrop();

    constexpr int W = 900, H = 470, PAD = 32, QR_BOX = 300, FOOT_H = 58;
    const int x = (SCREEN_W - W) / 2, y = (SCREEN_H - H) / 2;
    fillRounded(x, y, W, H, 22, T().panelBg);
    strokeRounded(x, y, W, H, 22, 1, T().panelBorder);

    // Left: the code on white, whatever the theme - scanners want contrast.
    const int qx = x + PAD, qy = y + PAD + 4;
    fillRounded(qx, qy, QR_BOX, QR_BOX, 14, SDL_Color{255, 255, 255, 255});
    if (qrcode) drawQrCode(renderer_, qrcode, qx, qy, QR_BOX);

    // Right: title, the two steps, the address, the status.
    const int tx = qx + QR_BOX + PAD, tw = x + W - PAD - tx;
    int ty = y + PAD;
    {
        TTF_Font* fTitle = uiFont(26, true);
        if (SDL_Texture* t = uiIcon("phone")) {
            SDL_SetTextureColorMod(t, T().accent.r, T().accent.g, T().accent.b);
            SDL_Rect dst = {tx, ty + 2, 28, 28};
            SDL_RenderCopy(renderer_, t, nullptr, &dst);
            SDL_SetTextureColorMod(t, 255, 255, 255);
        }
        drawText(i18n::get(StrKey::PhoneTitle), tx + 38, ty, T().text, fTitle);
        ty += 52;
    }

    TTF_Font* fBody = uiFont(16);
    TTF_Font* fNum  = uiFont(14, true);
    auto step = [&](int n, const std::string& text) {
        fillDisc(tx + 13, ty + 11, 13, T().badgeBg);
        drawTextCentered(std::to_string(n), tx + 13, ty + 11, T().accent, fNum);
        for (const auto& l : wrapText(text, fBody, tw - 40, 3)) {
            drawText(l, tx + 40, ty, T().text, fBody);
            ty += 22;
        }
        ty += 12;
    };
    step(1, i18n::get(StrKey::PhoneStep1));
    step(2, i18n::get(StrKey::PhoneStep2));

    // The address, on its own lines when it does not fit on one.
    {
        TTF_Font* fUrl = uiFont(17, true);
        const auto lines = wrapChars(url, fUrl, tw - 28, 3);
        // Each line centred on its row, the way the rest of the app centres
        // text: the font's line box is taller than its letters, so placing
        // it by its top edge leaves the address low in the box.
        constexpr int ROW = 26, PAD = 10;
        const int boxH = 2 * PAD + static_cast<int>(lines.size()) * ROW;
        fillRounded(tx, ty, tw, boxH, 10, T().bg);
        strokeRounded(tx, ty, tw, boxH, 10, 1, T().buttonBorder);
        int cy = ty + PAD + ROW / 2;
        for (const auto& l : lines) {
            drawText(l, tx + 14, cy - TTF_FontHeight(fUrl) / 2, T().accent, fUrl);
            cy += ROW;
        }
        ty += boxH + 18;
    }

    // Status: a dot that turns green once a phone has opened the page.
    {
        TTF_Font* fStatus = uiFont(15, true);
        const bool inUse = server.served() > 0;
        const SDL_Color c = inUse ? T().statusOk : T().textDim;
        fillDisc(tx + 6, ty + 10, 5, c);
        const std::string status = inUse
            ? i18n::fmt(StrKey::PhoneInUse, std::to_string(server.served()))
            : i18n::get(StrKey::PhoneWaiting);
        drawText(fitText(status, fStatus, tw - 20), tx + 20, ty + 10 - TTF_FontHeight(fStatus) / 2, c, fStatus);
        ty += 32;
    }

    // Under the code: what it is and is not.
    {
        TTF_Font* fNote = uiFont(13);
        int ny = qy + QR_BOX + 14;
        for (const auto& l : wrapText(i18n::get(StrKey::PhoneNote), fNote, W - 2 * PAD, 2)) {
            drawText(l, x + PAD, ny, T().textMuted, fNote);
            ny += 18;
        }
    }

    // Footer: the one key there is.
    {
        const int fy = y + H - FOOT_H;
        drawRect(x, fy, W, 1, T().panelBorder);
        const int cy = fy + FOOT_H / 2;
        TTF_Font* f = uiFont(15, true);
        int hx = x + 26;
        hx += drawFooterKey(hx, cy, "B", false) + 8;
        drawText(i18n::get(StrKey::PhoneStop), hx, cy - TTF_FontHeight(f) / 2, T().text, f);
    }
}
