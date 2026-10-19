
#define CROW_MAIN

#include <filesystem>
#include <thread>
#include <chrono>
#include "crow.h"
#include "nlohmann/json.hpp"
#include "toy.h"

using json = nlohmann::json;
namespace fs = std::filesystem;

#define state (*statePtr)

namespace {

    fs::path resolve_path(const std::string& raw_path, const fs::path& base_dir = fs::current_path()) {
        return (base_dir / raw_path).lexically_normal();
    }

    crow::response serve_static_file(const std::string& relative_path) {
        fs::path full_path = resolve_path(relative_path, staticPath);

        if (!fs::exists(full_path)) {
            return crow::response(404, "Not found: " + relative_path);
        }

        crow::response res;
        res.set_static_file_info_unsafe(full_path.string());
        return res;
    }

} // namespace


// ── Authentication middleware ─────────────────────────────────────────────────
// Validates the Bearer token on every request except registration

struct AuthMiddleware {
    struct context {
        uint32_t userId = UINT32_MAX;  // populated on successful auth
    };

    void before_handle(crow::request& req, crow::response& res, context& ctx) {
        // Endpoints that do not require authentication
        if (req.url == "/api/health")
            return;
        if (req.url == "/api/users" &&
            req.method == crow::HTTPMethod::POST)
            return;

        // Allow static file routes through without authentication
        if (req.url == "/" ||
            req.url == "/style.css" ||
            req.url == "/app.js" ||
            req.url == "/manifest.json" ||
            req.url == "/sw.js" ||
            req.url == "/icon.png")
            return;

        // All other routes require a valid Bearer token
        auto auth = req.get_header_value("Authorization");
        if (auth.empty() || auth.substr(0, 7) != "Bearer ") {
            res.code = 401;
            res.body = json({ { "error", "Missing or invalid Authorization header" } }).dump();
            res.end();
            return;
        }

        std::string tokenStr = auth.substr(7);
        uint64_t    token = 0;
        try {
            token = std::stoull(tokenStr, nullptr, 16);
        }
        catch (...) {
            res.code = 401;
            res.body = json({ { "error", "Invalid token format" } }).dump();
            res.end();
            return;
        }

        std::shared_lock lock(data_mutex);
        auto it = usersByToken.find(token);
        if (it == usersByToken.end()) {
            res.code = 401;
            res.body = json({ { "error", "Invalid token" } }).dump();
            res.end();
            return;
        }

        ctx.userId = it->second;
    }

    void after_handle(crow::request& /*req*/, crow::response& /*res*/, context& /*ctx*/) {}
};

using App = crow::App<AuthMiddleware>;

// ── Response helpers ──────────────────────────────────────────────────────────

crow::response OkResponse(const json& body, int code = 200) {
    crow::response res(code, body.dump());
    res.add_header("Content-Type", "application/json");
    return res;
}

crow::response ErrorResponse(int code, const std::string& message) {
    crow::response res(code, json({ { "error", message } }).dump());
    res.add_header("Content-Type", "application/json");
    return res;
}

// ── Map ToyError codes to HTTP responses ──────────────────────────────────────

crow::response ToyErrorResponse(int result) {
    switch (result) {
    case ToyError::DUPLICATE_KEY:          return ErrorResponse(409, "Name already exists");
    case ToyError::NAME_TOO_LONG:          return ErrorResponse(422, "Name too long");
    case ToyError::INSUFFICIENT_CAPACITY:  return ErrorResponse(507, "Server at capacity");
    case ToyError::INVALID_INDEX:          return ErrorResponse(404, "Resource not found");
    case ToyError::DUPLICATE_GAME:         return ErrorResponse(409, "Game already exists between these players");
    case ToyError::GAME_LIMIT_REACHED:     return ErrorResponse(403, "Player has reached the game limit");
    case ToyError::USER_INACTIVE:          return ErrorResponse(403, "Player is inactive");
    case ToyError::GAME_INACTIVE:          return ErrorResponse(403, "Game is inactive");
    case ToyError::RATE_LIMITED:           return ErrorResponse(429, "One event per hour per player per game");
    case ToyError::GAME_NOT_PENDING:       return ErrorResponse(409, "Invitation already accepted");
    case ToyError::NOT_INVITED:            return ErrorResponse(403, "Only the invited player can accept");
    default:                               return ErrorResponse(500, "Internal error");
    }
}

// ── Route registration ────────────────────────────────────────────────────────

void RegisterRoutes(App& app) {

    // ── Health check ──────────────────────────────────────────────────────────
    // GET /api/health
    // No authentication required — useful for monitoring and load balancer checks

    CROW_ROUTE(app, "/api/health")
        ([]() {
        std::shared_lock lock(data_mutex);
        return OkResponse({
            { "status",     "ok"              },
            { "userCount",  state.userCount   },
            { "gameCount",  state.gameCount   },
            { "eventCount", state.eventCount  }
            });
            });

    // ── Register a new user ───────────────────────────────────────────────────
    // POST /api/users
    // Body: { "name": "Eric" }
    // Returns: { "id": 0, "name": "Eric", "token": "a1b2c3d4e5f6a7b8" }
    // No authentication required

    CROW_ROUTE(app, "/api/users").methods(crow::HTTPMethod::POST)
        ([](const crow::request& req) {
        auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.contains("name"))
            return ErrorResponse(400, "Request body must contain 'name'");

        std::string name = body["name"].get<std::string>();
        uint32_t    id = 0;
        int result = AddUser(std::string_view(name), id);

        if (result != ToyError::SUCCESS)
            return ToyErrorResponse(result);

        // Return token as hex string — stored by PWA for all future requests
        char tokenStr[17];
        snprintf(tokenStr, sizeof(tokenStr), "%016llx",
            static_cast<unsigned long long>(users[id].token));

        return OkResponse({
            { "id",    id       },
            { "name",  name     },
            { "token", tokenStr }
          }, 201);
        });

    // ── Get user profile ──────────────────────────────────────────────────────
    // GET /api/users/{id}
    // Returns basic profile for the requesting user

    CROW_ROUTE(app, "/api/users/<uint>")
        ([&app](const crow::request& req, uint32_t userId) {
        auto& ctx = app.get_context<AuthMiddleware>(req);
        if (ctx.userId != userId)
            return ErrorResponse(403, "Access denied");

        std::shared_lock lock(data_mutex);
        if (userId >= state.userCount)
            return ErrorResponse(404, "User not found");

        return OkResponse({
            { "id",        userId                          },
            { "name",      users[userId].name.data()       },
            { "status",    users[userId].status            },
            { "gameCount", gamesByUser[userId].size()      }
            });
            });

    // ── Who am I ─────────────────────────────────────────────────────────────
    // GET /api/me
    // Resolves identity from the Bearer token alone. This is what lets a
    // second device restore a session from just a pasted token, without
    // needing the full {id, name, token} blob — the token alone is enough
    // to look up the rest.

    CROW_ROUTE(app, "/api/me")
        ([&app](const crow::request& req) {
        auto& ctx = app.get_context<AuthMiddleware>(req);
        if (ctx.userId == UINT32_MAX)
            return ErrorResponse(401, "Unauthorized");

        std::shared_lock lock(data_mutex);
        return OkResponse({
            { "id",   ctx.userId                      },
            { "name", users[ctx.userId].name.data()   }
            });
            });

    // ── Create a game ─────────────────────────────────────────────────────────
    // POST /api/games
    // Body: { "partnerId": 1 }
    // The requesting user's ID comes from the auth token context
    // Returns: { "id": 0, "player1": 0, "player2": 1 }

    CROW_ROUTE(app, "/api/games").methods(crow::HTTPMethod::POST)
        ([&app](const crow::request& req) {
        auto& ctx = app.get_context<AuthMiddleware>(req);
        if (ctx.userId == UINT32_MAX)
            return ErrorResponse(401, "Unauthorized");

        auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.contains("partnerId"))
            return ErrorResponse(400, "Request body must contain 'partnerId'");

        uint32_t partnerId = body["partnerId"].get<uint32_t>();
        uint32_t gameId = 0;
        int result = AddGame(ctx.userId, partnerId, gameId);

        if (result != ToyError::SUCCESS)
            return ToyErrorResponse(result);

        std::shared_lock lock(data_mutex);
        return OkResponse({
            { "id",          gameId               },
            { "player1",     ctx.userId           },
            { "player1Name", users[ctx.userId].name.data() },
            { "player2",     partnerId            },
            { "player2Name", users[partnerId].name.data()  },
            { "status",      Status::PENDING_INVITE }
            }, 201);
            });

    // ── Get games for the authenticated user ──────────────────────────────────
    // GET /api/games

    CROW_ROUTE(app, "/api/games")
        ([&app](const crow::request& req) {
        auto& ctx = app.get_context<AuthMiddleware>(req);
        if (ctx.userId == UINT32_MAX)
            return ErrorResponse(401, "Unauthorized");

        std::shared_lock lock(data_mutex);
        json res = json::array();

        auto it = gamesByUser.find(ctx.userId);
        if (it != gamesByUser.end()) {
            for (uint32_t gameId : it->second) {
                const Game& g = games[gameId];
                res.push_back({
                    { "id",          gameId                          },
                    { "player1",     g.player1Id                     },
                    { "player1Name", users[g.player1Id].name.data()  },
                    { "player2",     g.player2Id                     },
                    { "player2Name", users[g.player2Id].name.data()  },
                    { "status",      g.status                        }
                });
            }
        }
        return OkResponse(res);
            });

    // ── Accept a game invitation ──────────────────────────────────────────────
    // POST /api/games/{id}/accept

    CROW_ROUTE(app, "/api/games/<uint>/accept").methods(crow::HTTPMethod::POST)
        ([&app](const crow::request& req, uint32_t gameId) {
        auto& ctx = app.get_context<AuthMiddleware>(req);
        if (ctx.userId == UINT32_MAX)
            return ErrorResponse(401, "Unauthorized");

        int result = AcceptGame(gameId, ctx.userId);
        if (result != ToyError::SUCCESS)
            return ToyErrorResponse(result);

        std::shared_lock lock(data_mutex);
        const Game& g = games[gameId];
        return OkResponse({
            { "id",     gameId   },
            { "status", g.status }
            });
            });

    // ── Post an event ─────────────────────────────────────────────────────────
    // POST /api/games/{id}/events

    CROW_ROUTE(app, "/api/games/<uint>/events").methods(crow::HTTPMethod::POST)
        ([&app](const crow::request& req, uint32_t gameId) {
        auto& ctx = app.get_context<AuthMiddleware>(req);
        if (ctx.userId == UINT32_MAX)
            return ErrorResponse(401, "Unauthorized");

        // Timestamp is set server-side — clients cannot manipulate it
        int64_t  timestamp = static_cast<int64_t>(std::time(nullptr));
        uint32_t eventId = 0;
        int result = AddEvent(gameId, ctx.userId, timestamp, eventId);

        if (result != ToyError::SUCCESS)
            return ToyErrorResponse(result);

        // Build response — include match information if a match occurred
        json res = {
            { "id",      eventId                              },
            { "gameId",  gameId                               },
            { "status",  events[eventId].status               },
            { "matched", events[eventId].matchedEventId != EventStatus::UNMATCHED }
        };

        if (events[eventId].matchedEventId != EventStatus::UNMATCHED) {
            uint32_t matchId = events[eventId].matchedEventId;
            int64_t  diff = std::abs(timestamp - events[matchId].timestamp);
            uint32_t score = static_cast<uint32_t>(MATCH_SPAN_SECONDS - diff);

            res["matchedEventId"] = matchId;
            res["score"] = score;
        }

        return OkResponse(res, 201);
            });

    // ── Get events for a game ─────────────────────────────────────────────────
    // GET /api/games/{id}/events

    CROW_ROUTE(app, "/api/games/<uint>/events")
        ([&app](const crow::request& req, uint32_t gameId) {
        auto& ctx = app.get_context<AuthMiddleware>(req);
        if (ctx.userId == UINT32_MAX)
            return ErrorResponse(401, "Unauthorized");

        std::shared_lock lock(data_mutex);

        if (gameId >= state.gameCount)
            return ErrorResponse(404, "Game not found");

        // Verify the requesting user is a participant in this game
        const Game& g = games[gameId];
        if (g.player1Id != ctx.userId && g.player2Id != ctx.userId)
            return ErrorResponse(403, "Access denied");

        auto it = eventsByGame.find(gameId);
        json res = json::array();

        if (it != eventsByGame.end()) {
            for (uint32_t eventId : it->second) {
                const GameEvent& ev = events[eventId];

                json entry = {
                    { "id",        eventId       },
                    { "userId",    ev.userId     },
                    { "ts",        ev.timestamp  },
                    { "status",    ev.status     },
                    { "matched",   ev.matchedEventId != EventStatus::UNMATCHED }
                };

                // Include score if this event is part of a match
                if (ev.matchedEventId != EventStatus::UNMATCHED) {
                    int64_t diff = std::abs(
                        ev.timestamp - events[ev.matchedEventId].timestamp);
                    entry["score"] = MATCH_SPAN_SECONDS - diff;
                    entry["matchedEventId"] = ev.matchedEventId;
                }

                res.push_back(entry);
            }
        }
        return OkResponse(res);
            });

    // ── Get notifications for the authenticated user ───────────────────────────
    // GET /api/notifications

    CROW_ROUTE(app, "/api/notifications")
        ([&app](const crow::request& req) {
        auto& ctx = app.get_context<AuthMiddleware>(req);
        if (ctx.userId == UINT32_MAX)
            return ErrorResponse(401, "Unauthorized");

        std::shared_lock lock(data_mutex);
        json res = GetPendingNotifications(ctx.userId);
        return OkResponse(res);
            });

    // ── Look up a user by name ────────────────────────────────────────────────
    // GET /api/users/search?name=Eric

    CROW_ROUTE(app, "/api/users/search")
        ([](const crow::request& req) {
        auto name = req.url_params.get("name");
        if (!name)
            return ErrorResponse(400, "Query parameter 'name' is required");

        std::shared_lock lock(data_mutex);
        auto it = usersByName.find(std::string_view(name));
        if (it == usersByName.end())
            return ErrorResponse(404, "User not found");

        uint32_t userId = it->second;
        return OkResponse({
            { "id",   userId                      },
            { "name", users[userId].name.data()   }
            });
            });

    // ── Static PWA files ──────────────────────────────────────────────────────

    CROW_ROUTE(app, "/")
        ([]() {
        return serve_static_file("index.html");
            });

    CROW_ROUTE(app, "/style.css")
        ([]() {
        return serve_static_file("style.css");
            });

    CROW_ROUTE(app, "/app.js")
        ([]() {
        return serve_static_file("app.js");
            });

    CROW_ROUTE(app, "/manifest.json")
        ([]() {
        return serve_static_file("manifest.json");
            });

    CROW_ROUTE(app, "/sw.js")
        ([]() {
        return serve_static_file("sw.js");
            });

    CROW_ROUTE(app, "/icon.png")
        ([]() {
        return serve_static_file("icon.png");
            });

    // Catch-all — diagnose unmatched routes
    CROW_ROUTE(app, "/<path>")
        ([](const crow::request& req, std::string path) {
        std::string method_str = crow::method_name(req.method);
        printf("[debug] Unmatched route: %s %s\n",
            method_str.c_str(),
            req.url.c_str());
        crow::response res(404, "Not found: " + req.url);
        return res;
            });
}

// ── Entry point ───────────────────────────────────────────────────────────────

int main() {

    // Disable stdout buffering so logs stream instantly to systemd
    std::setvbuf(stdout, NULL, _IONBF, 0);

    // If launched by a daemon where CWD becomes "/", reset it to the binary location
    if (fs::current_path() == "/") {
        try {
            fs::path binary_dir = fs::read_symlink("/proc/self/exe").parent_path();
            fs::current_path(binary_dir);
        }
        catch (const fs::filesystem_error& e) {
            printf("Warning: failed to reset CWD from binary location: %s\n", e.what());
        }
    }

    App app;
    RegisterRoutes(app);

    uint16_t    port       = 8088;
    uint16_t    threads    = 2;
    std::string certFile   = "";
	std::string keyFile    = "";
	std::string dataFile   = "toy.dat";

    std::string config_path_raw = GetEnv("TOY_CONFIG").value_or("settings.json");
    fs::path settingsFile = resolve_path(config_path_raw);
    
    if (fs::exists(settingsFile)) {
        try {
            std::ifstream file(settingsFile);
            json          data = json::parse(file);

            if (data.contains("port"))        port       = data["port"].get<uint16_t>();
            if (data.contains("threads"))     threads    = data["threads"].get<uint16_t>();
            if (data.contains("cert_file"))   certFile   = data["cert_file"].get<std::string>();
            if (data.contains("key_file"))    keyFile    = data["key_file"].get<std::string>();
            if (data.contains("data_file"))   dataFile   = data["data_file"].get<std::string>();
            if (data.contains("static_path")) staticPath = data["static_path"].get<std::string>();

            if (port == 0)    port    = 8088; 
            if (threads == 0) threads = 2;
            if (threads > 16) threads = 16;
        }
        catch (const std::exception& e) {
            printf("[settings] Failed to parse settings.json: %s — using defaults\n",
                e.what());
        }
    }
    else {
        printf("[settings] settings.json not found at %s — using defaults\n", settingsFile.string().c_str());
    }

    if (!OpenDataFile(resolve_path(dataFile))) {
        printf("Failed to open data file at: %s — exiting\n", dataFile.c_str());
        return 1;
    }

    // ── Validate TLS configuration ────────────────────────────────────────────
    // is_regular_file (not just exists) also catches a path that resolves to
    // a directory rather than a file — a typo'd certFile pointing at a folder
    // would otherwise pass an exists()-only check. Fully cross-platform via
    // std::filesystem, no #ifdef needed here unlike MappedFile's raw APIs.
    bool useTLS = false;
    if (!certFile.empty() && !keyFile.empty()) {
        bool certFound = fs::is_regular_file(certFile);
        bool keyFound  = fs::is_regular_file(keyFile);

        if (certFound && keyFound) {
            useTLS = true;
        } else {
            if (!certFound) printf("[settings] cert_file not found: %s\n", certFile.c_str());
            if (!keyFound)  printf("[settings] key_file not found: %s\n",  keyFile.c_str());
            printf("[settings] TLS was configured but the cert/key could not be found — falling back to HTTP\n");
        }
    }

    // ── Sweep thread ─────────────────────────────────────────────────────────
    // Marks PENDING, unmatched events past their window as EXPIRED. This is
    // v1's whole notification story: no push yet, just accurate state for a
    // player to find when they check back on their own. Detached — runs for
    // the server's full lifetime, same as the app itself.
    std::thread sweepThread([]() {
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(SWEEP_INTERVAL_SECONDS));
            int count = SweepExpiredEvents(std::time(nullptr));
            if (count > 0) {
                printf("[sweep] marked %d event(s) expired\n", count);
            }
        }
        });
    sweepThread.detach();

    if (useTLS) {
        printf("ToY server running on https://localhost:%u using %u threads\n",
            port, threads);
        app.ssl_file(certFile, keyFile)
           .port(port)
           .concurrency(threads)
           .run();
    }
    else {
        printf("ToY server running on http://localhost:%u using %u threads\n",
            port, threads);
        app.port(port)
           .concurrency(threads)
           .run();
    }

    CloseDataFile();
    return 0;
}
