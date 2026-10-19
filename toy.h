#ifndef TOY_H
#define TOY_H

#pragma once
#include <stdint.h>
#include <iostream>
#include <time.h>
#include <cstdint>
#include <cstring>
#include <array>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <cstdio>
#include <algorithm>
#include <shared_mutex>
#include <mutex>
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <optional>
#include <string>
#include "toy-storage.h"
#include <filesystem> // for OpenDataFile

using json = nlohmann::json;
namespace fs = std::filesystem; // for OpenDataFile

// C++17 Inline Variable: Safe to put directly in the header
// ── Constants ────────────────────────────────────────────────────────────────

inline constexpr uint32_t USER_CAPACITY          = 500;
inline constexpr uint32_t GAME_CAPACITY          = 1000;
inline constexpr uint32_t EVENT_CAPACITY         = 50000;
inline constexpr uint32_t MATCH_SPAN_SECONDS     = 3600;
inline constexpr uint32_t SWEEP_INTERVAL_SECONDS = 60; // how often expired events are swept
inline constexpr uint32_t MAX_GAMES_PER_PLAYER   = 10;
inline constexpr uint32_t NAME_LEN               = 28;

// Status flag definitions — shared across User and Game
namespace Status {
    inline constexpr uint32_t ACTIVE         = 0x00000000; // default state
    inline constexpr uint32_t INACTIVE       = 0x00000001; // no longer participating
    inline constexpr uint32_t AT_GAME_LIMIT  = 0x00000002; // reached max games
    inline constexpr uint32_t USER_DELETED   = 0x00000004; // marked by user
    inline constexpr uint32_t ARCHIVED       = 0x00000008; // moved to archive
    inline constexpr uint32_t SUSPENDED      = 0x00000010; // administrative hold
    inline constexpr uint32_t PENDING_INVITE = 0x00000020; // game created, awaiting player2's acceptance
}

namespace EventStatus { // proposed
    inline constexpr uint32_t PENDING   = 0x00000000;  // awaiting match or expiry
    inline constexpr uint32_t MATCHED   = 0x00000001;  // match found, both notified
    inline constexpr uint32_t EXPIRED   = 0x00000002;  // hour passed, other notified
    inline constexpr uint32_t NOTIFIED  = 0x00000004;  // expiry notification sent
    inline constexpr uint32_t UNMATCHED = UINT32_MAX;  // special value for unmatched events
}

// ── Error codes ──────────────────────────────────────────────────────────────

namespace ToyError {
    inline constexpr int SUCCESS =                0;
    inline constexpr int INSUFFICIENT_CAPACITY = -1;
    inline constexpr int NAME_TOO_LONG =         -2;
    inline constexpr int DUPLICATE_KEY =         -3;
    inline constexpr int GAME_INACTIVE =         -4;
    inline constexpr int USER_INACTIVE =         -5;
    inline constexpr int RATE_LIMITED =          -6;
    inline constexpr int INVALID_INDEX =         -7;
    inline constexpr int GAME_LIMIT_REACHED =    -8;
    inline constexpr int DUPLICATE_GAME =        -9;
    inline constexpr int GAME_NOT_PENDING =     -10; // game already accepted, or was never pending
    inline constexpr int NOT_INVITED =          -11; // caller is not the invited player (player2)
}

// ── Data structures ──────────────────────────────────────────────────────────

struct alignas(64) User {
    std::array<char, 28> name;        // 28 bytes
    uint32_t             status;      //  4 bytes
    uint64_t             token;       //  8 bytes
    uint8_t              padding[24]; // 24 bytes — reserved
};                                    // total: 64 bytes

struct alignas(16) Game {
    uint32_t player1Id;  // 4 bytes
    uint32_t player2Id;  // 4 bytes
    uint32_t status;     // 4 bytes — bitmask field
    uint32_t padding;    // 4 bytes — reserved for future use
};                       // total: 16 bytes

struct alignas(32) GameEvent {
    uint32_t gameId;           // 4 bytes
    uint32_t userId;           // 4 bytes
    int64_t  timestamp;        // 8 bytes
    uint32_t matchedEventId;   // 4 bytes
    uint32_t status;           // 4 bytes — new, replaces 4 bytes of padding
    uint8_t  padding[8];       // 8 bytes — was 12, now 8
};                             // total: 32 bytes — unchanged

// Maintain in file header or a global state struct
struct TableState {
    uint32_t userCount;  // next free index in users[]
    uint32_t gameCount;  // next free index in games[]
    uint32_t eventCount; // next free index in events[]
};

// Canonical pair key — always ordered smaller ID first
struct PlayerPair {
    uint32_t lowId;
    uint32_t highId;

    PlayerPair(uint32_t a, uint32_t b)
        : lowId(std::min(a, b)), highId(std::max(a, b)) {
    }

    bool operator==(const PlayerPair& other) const {
        return lowId == other.lowId && highId == other.highId;
    }
};

// Hash for PlayerPair
struct PlayerPairHash {
    size_t operator()(const PlayerPair& p) const {
        // Combine both IDs into a single 64-bit value and hash it
        uint64_t combined = (static_cast<uint64_t>(p.lowId) << 32) | p.highId;
        return std::hash<uint64_t>{}(combined);
    }
};

// ── Global state- ─────────────────────────────────────────────────────────────
inline std::filesystem::path staticPath;
inline MappedFile            dataFile;
inline User*                 users;
inline Game*                 games;
inline GameEvent*            events;
inline TableState*           statePtr;

// ── Thread safety ─────────────────────────────────────────────────────────────
inline std::shared_mutex data_mutex;

// ── Indexes (in memory only) ─────────────────────-────────────────────────────

inline std::unordered_map<std::string_view, uint32_t>           usersByName;
inline std::unordered_map<uint64_t, uint32_t>                   usersByToken;
inline std::unordered_map<uint32_t, std::vector<uint32_t>>      gamesByUser;
inline std::unordered_map<PlayerPair, uint32_t, PlayerPairHash> gamesByPair;
inline std::unordered_map<uint32_t, std::vector<uint32_t>>      eventsByGame;
inline std::unordered_map<uint64_t, int64_t>                    lastEventByPlayerGame;

// ── Functions ────────────────────────────────────────────────────────────────

bool OpenDataFile(const fs::path& path);
void CloseDataFile();
uint64_t PlayerGameKey(uint32_t playerId, uint32_t gameId);
int RebuildIndexes();
int Initialize();
uint64_t GenerateToken(void);
json GetPendingNotifications(uint32_t userId);
void SetUserStatus(uint32_t userId, uint32_t flags);
void ClearUserStatus(uint32_t userId, uint32_t flags);
bool UserHasStatus(uint32_t userId, uint32_t flags);
bool IsUserActive(uint32_t userId);
void SetGameStatus(uint32_t gameId, uint32_t flags);
bool IsGameActive(uint32_t gameId);
const auto& GetGamesForPlayer(uint32_t playerId);
const auto& GetEventsForGame(uint32_t gameId);
bool IsEventAllowed(uint32_t playerId, uint32_t gameId, int64_t timestamp);
uint32_t FindMatchableEvent(uint32_t gameId, uint32_t playerId, int64_t timestamp);
int AddUser(std::string_view userName, uint32_t& userIdOut);
int AddGame(uint32_t userId1, uint32_t userId2, uint32_t& gameIdOut);
int AcceptGame(uint32_t gameId, uint32_t playerId);
int AddEvent(uint32_t gameId, uint32_t userId, int64_t timestamp, uint32_t& eventIdOut);
int SweepExpiredEvents(int64_t now);

// ── Helper ───────────────────────────────────────────────────────────────────

const char* ErrorString(int code);
void PrintResult(std::string_view label, int result, uint32_t id = UINT32_MAX);

// 'inline' keyword allows inclusion in multiple .cpp files without causing a "multiple definition" linker error.
inline std::optional<std::string> GetEnv(const std::string& name) {
    if (const char* val = std::getenv(name.c_str())) {
        return std::string(val);
    }
    return std::nullopt;
}

#endif
