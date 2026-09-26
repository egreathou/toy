
#include <iostream>
#include <time.h>
//#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <array>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <cstdio>
#include <algorithm>
#include <shared_mutex>
#include <random>
#include "toy.h"
#include "toy-storage.h"

#ifdef TOY_STATIC_STORAGE
static User       staticUsers[USER_CAPACITY];
static Game       staticGames[GAME_CAPACITY];
static GameEvent  staticEvents[EVENT_CAPACITY];
static TableState staticState;
#endif

#define state (*statePtr)

// ── File layout ───────────────────────────────────────────────────────────────
// Offset 0                   : TableState
// Offset sizeof(TableState)  : User[USER_CAPACITY]     (rounded up to alignof(User))
// Offset above + users size  : Game[GAME_CAPACITY]     (rounded up to alignof(Game))
// Offset above + games size  : GameEvent[EVENT_CAPACITY] (rounded up to alignof(GameEvent))
//
// Each section is padded up to its struct's alignas() requirement. Without
// this, e.g. sizeof(TableState) == 12 leaves every later array 12 bytes off
// from the alignment its alignas() declares — harmless-looking on some
// structs, but a real SIGSEGV on GameEvent, whose 32-byte aggregate init
// gets compiled to an aligned SIMD store that faults on a misaligned address.

static constexpr size_t AlignUp(size_t value, size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

static constexpr size_t FILE_OFFSET_STATE = 0;
static constexpr size_t FILE_OFFSET_USERS = AlignUp(FILE_OFFSET_STATE + sizeof(TableState), alignof(User));
static constexpr size_t FILE_OFFSET_GAMES = AlignUp(FILE_OFFSET_USERS + sizeof(User) * USER_CAPACITY, alignof(Game));
static constexpr size_t FILE_OFFSET_EVENTS = AlignUp(FILE_OFFSET_GAMES + sizeof(Game) * GAME_CAPACITY, alignof(GameEvent));
static constexpr size_t FILE_TOTAL_SIZE = FILE_OFFSET_EVENTS + sizeof(GameEvent) * EVENT_CAPACITY;

// ── Data File Handling ────────────────────────────────────────────────────────

bool OpenDataFile(const fs::path& path) {
    bool isNew = false;

    if (!dataFile.open(path.string().c_str(), FILE_TOTAL_SIZE, isNew)) {
        printf("[toy] Failed to open data file: %s\n", path.string().c_str());
        return false;
    }

    // Point table pointers into the mapped memory
    uint8_t* base = static_cast<uint8_t*>(dataFile.data());
    statePtr = reinterpret_cast<TableState*>(base + FILE_OFFSET_STATE);
    users = reinterpret_cast<User*>      (base + FILE_OFFSET_USERS);
    games = reinterpret_cast<Game*>      (base + FILE_OFFSET_GAMES);
    events = reinterpret_cast<GameEvent*> (base + FILE_OFFSET_EVENTS);

    if (isNew) {
        // First run — zero everything and initialize state
        // Clear indexes first while pointers are valid but before memset
        usersByName.clear();
        usersByToken.clear();
        gamesByUser.clear();
        gamesByPair.clear();
        eventsByGame.clear();
        lastEventByPlayerGame.clear();

        std::memset(dataFile.data(), 0, FILE_TOTAL_SIZE);
        statePtr->userCount = 0;
        statePtr->gameCount = 0;
        statePtr->eventCount = 0;

        printf("[toy] New data file created: %s\n", path.string().c_str());
    }
    else {
        // Existing file — rebuild indexes from persisted data
        printf("[toy] Opened existing data file: %s users=%u  games=%u  events=%u\n",
            path.string().c_str(),
            statePtr->userCount,
            statePtr->gameCount,
            statePtr->eventCount);
        RebuildIndexes();
    }

    // Reserve index capacity
    usersByName.reserve(USER_CAPACITY);
    usersByToken.reserve(USER_CAPACITY);
    gamesByUser.reserve(USER_CAPACITY);
    gamesByPair.reserve(GAME_CAPACITY);
    eventsByGame.reserve(GAME_CAPACITY);
    lastEventByPlayerGame.reserve(GAME_CAPACITY * 2);

    return true;
}

void CloseDataFile() {
    dataFile.flush();
    dataFile.close();
}

// ── Utility Functions ─────────────────────────────────────────────────────────

uint64_t PlayerGameKey(uint32_t playerId, uint32_t gameId) {
    return (static_cast<uint64_t>(playerId) << 32) | gameId;
}

int RebuildIndexes() {

    // rebuild usersByName index
    usersByName.clear();
    for (uint32_t i = 0; i < state.userCount; ++i)
        usersByName.try_emplace(
            std::string_view(users[i].name.data(),
                strnlen(users[i].name.data(), NAME_LEN)),
            i
        );

	// rebuild usersByToken index
    usersByToken.clear();
    for (uint32_t i = 0; i < state.userCount; ++i)
        if (users[i].token != 0)
            usersByToken[users[i].token] = i;

    // rebuild gamesByUser and gamesByPair indexes
    gamesByUser.clear();
    gamesByPair.clear();
    for (uint32_t i = 0; i < state.gameCount; ++i) {
        const Game& game = games[i];
        gamesByUser[game.player1Id].push_back(i);
        gamesByUser[game.player2Id].push_back(i);
        gamesByPair.emplace(PlayerPair(game.player1Id, game.player2Id), i);
    }

    // rebuild eventsByGame and lastEventByPlayerGame indexes
    eventsByGame.clear();
    lastEventByPlayerGame.clear();
    for (uint32_t i = 0; i < state.eventCount; ++i) {
        const GameEvent& ev = events[i];
        eventsByGame[ev.gameId].push_back(i);
        uint64_t key = PlayerGameKey(ev.userId, ev.gameId);
        auto it = lastEventByPlayerGame.find(key);
        if (it == lastEventByPlayerGame.end() || ev.timestamp > it->second)
            lastEventByPlayerGame[key] = ev.timestamp;
    }

    return ToyError::SUCCESS;
}

int Initialize() {
#ifdef TOY_STATIC_STORAGE
    // Point global pointers at static storage
    // Safe to do before clearing indexes since no string_views exist yet
    users    = staticUsers;
    games    = staticGames;
    events   = staticEvents;
    statePtr = &staticState;
#endif

    // Clear indexes first — while string_view keys still point at valid memory
    usersByName          .clear();
    gamesByUser          .clear();
    gamesByPair          .clear();
    eventsByGame         .clear();
    lastEventByPlayerGame.clear();

    // Now safe to zero the tables
    std::memset(users,  0, sizeof(User)      * USER_CAPACITY);
    std::memset(games,  0, sizeof(Game)      * GAME_CAPACITY);
    std::memset(events, 0, sizeof(GameEvent) * EVENT_CAPACITY);
    std::memset(&state, 0, sizeof(TableState));

    // RebuildIndexes finds empty tables — nothing to rebuild
    // Reserve capacity for normal operation
    usersByName          .reserve(USER_CAPACITY);
    usersByToken         .reserve(USER_CAPACITY);
    gamesByUser          .reserve(USER_CAPACITY);
    gamesByPair          .reserve(GAME_CAPACITY);
    eventsByGame         .reserve(GAME_CAPACITY);
    lastEventByPlayerGame.reserve(GAME_CAPACITY * 2);

    return ToyError::SUCCESS;
}

// ── Token generation ───────────────────────────────────────────────────────────

// Called only from within AddUser — no lock needed, caller holds unique_lock
uint64_t GenerateToken() {
    std::random_device              rd;
    std::mt19937_64                 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;
    return dist(gen);
}

// ── Notifications ─────────────────────────────────────────────────────────────

// Stub — returns empty array until notification worker is implemented
// Called from within a shared_lock in the notifications route
json GetPendingNotifications(uint32_t userId) {
    return json::array();
}

// ── User status ───────────────────────────────────────────────────────────────

void SetUserStatus(uint32_t userId, uint32_t flags) {
    std::unique_lock lock(data_mutex);
    users[userId].status |= flags;
}

void ClearUserStatus(uint32_t userId, uint32_t flags) {
    std::unique_lock lock(data_mutex);
    users[userId].status &= ~flags;
}

bool UserHasStatus(uint32_t userId, uint32_t flags) {
    std::shared_lock lock(data_mutex);
    return (users[userId].status & flags) == flags;
}

bool IsUserActive(uint32_t userId) {
    std::shared_lock lock(data_mutex);
    return users[userId].status == Status::ACTIVE;
}

// ── Game status ───────────────────────────────────────────────────────────────

void SetGameStatus(uint32_t gameId, uint32_t flags) {
    std::unique_lock lock(data_mutex);
    games[gameId].status |= flags;
}

bool IsGameActive(uint32_t gameId) {
    std::shared_lock lock(data_mutex);
    return games[gameId].status == Status::ACTIVE;
}

// ── Event status ──────────────────────────────────────────────────────────────

// O(1) lookup, returns the list directly
const auto& GetGamesForPlayer(uint32_t playerId) {
    return gamesByUser[playerId];   // was gameIndex[playerId]
}

const auto& GetEventsForGame(uint32_t gameId) {
    return eventsByGame[gameId];    // was eventIndex[gameId]
}

bool IsEventAllowed(uint32_t playerId, uint32_t gameId, int64_t timestamp) {
    uint64_t key = PlayerGameKey(playerId, gameId);
    auto it = lastEventByPlayerGame.find(key);
    if (it == lastEventByPlayerGame.end()) return true;
    return (timestamp - it->second) >= MATCH_SPAN_SECONDS;
}

uint32_t FindMatchableEvent(uint32_t gameId, uint32_t userid, int64_t timestamp) {
    const auto& eventList = eventsByGame[gameId];

    for (uint32_t i = static_cast<uint32_t>(eventList.size()); i-- > 0; ) {
        uint32_t   eid = eventList[i];
        GameEvent& ev = events[eid];

        if (ev.userId == userid)        continue;
        if (ev.matchedEventId != EventStatus::UNMATCHED) continue;

        int64_t diff = std::abs(timestamp - ev.timestamp);
        if (diff > 3600)                    break;

        return eid;
    }
    return EventStatus::UNMATCHED;
}

// ── Add objects ───────────────────────────────────────────────────────────────

int AddUser(std::string_view userName, uint32_t& userIdOut) {
    std::unique_lock lock(data_mutex);

    if (state.userCount >= USER_CAPACITY)  return ToyError::INSUFFICIENT_CAPACITY;
    if (userName.size() >= NAME_LEN)       return ToyError::NAME_TOO_LONG;
    if (usersByName.contains(userName))    return ToyError::DUPLICATE_KEY;

    uint32_t index = state.userCount;

    std::memset(&users[index], 0, sizeof(User));
    std::copy_n(userName.data(), userName.size(), users[index].name.data());
    users[index].name[userName.size()] = '\0';
    users[index].status = Status::ACTIVE;
    users[index].token = GenerateToken();
    usersByToken[users[index].token] = index;  // token index must be updated here

    auto [iterator, inserted] = usersByName.try_emplace(
        std::string_view(users[index].name.data(), userName.size()),
        index
    );

    if (!inserted) return ToyError::DUPLICATE_KEY;

    userIdOut = state.userCount++;
    return ToyError::SUCCESS;
}   

int AddGame(uint32_t userId1, uint32_t userId2, uint32_t& gameIdOut) {
    std::unique_lock lock(data_mutex);

    if (state.gameCount >= GAME_CAPACITY)                    return ToyError::INSUFFICIENT_CAPACITY;
    if (userId1 >= state.userCount)                          return ToyError::INVALID_INDEX;
    if (userId2 >= state.userCount)                          return ToyError::INVALID_INDEX;
    if (userId1 == userId2)                                  return ToyError::INVALID_INDEX;

	// Direct field access — no nested lock calls it IsUserActive() to avoid deadlock
    if (gamesByUser[userId1].size() >= MAX_GAMES_PER_PLAYER) return ToyError::GAME_LIMIT_REACHED;
    if (gamesByUser[userId2].size() >= MAX_GAMES_PER_PLAYER) return ToyError::GAME_LIMIT_REACHED;
    if (users[userId1].status != Status::ACTIVE)             return ToyError::USER_INACTIVE;
    if (users[userId2].status != Status::ACTIVE)             return ToyError::USER_INACTIVE;
    if (gamesByPair.contains(PlayerPair(userId1, userId2)))  return ToyError::DUPLICATE_GAME;

    uint32_t id = state.gameCount++;
    games[id] = { userId1, userId2, Status::PENDING_INVITE, 0 };

    gamesByUser[userId1].push_back(id);
    gamesByUser[userId2].push_back(id);
	gamesByPair[PlayerPair(userId1, userId2)] = id;

    // Mark players at the limit as inactive for new games
    if (gamesByUser[userId1].size() >= MAX_GAMES_PER_PLAYER)
        users[userId1].status |= Status::AT_GAME_LIMIT;
    if (gamesByUser[userId2].size() >= MAX_GAMES_PER_PLAYER)
        users[userId2].status |= Status::AT_GAME_LIMIT;

    gameIdOut = id;
    return ToyError::SUCCESS;
}

int AcceptGame(uint32_t gameId, uint32_t playerId) {
    std::unique_lock lock(data_mutex);

    if (gameId >= state.gameCount)                        return ToyError::INVALID_INDEX;

    Game& g = games[gameId];
    if (!(g.status & Status::PENDING_INVITE))              return ToyError::GAME_NOT_PENDING;
    if (g.player2Id != playerId)                           return ToyError::NOT_INVITED;

    g.status &= ~Status::PENDING_INVITE;
    return ToyError::SUCCESS;
}

int AddEvent(uint32_t gameId, uint32_t userId, int64_t timestamp, uint32_t& eventIdOut) {
    std::unique_lock lock(data_mutex);

    if (state.eventCount >= EVENT_CAPACITY)         return ToyError::INSUFFICIENT_CAPACITY;
    if (gameId >= state.gameCount)                  return ToyError::INVALID_INDEX;
    if (userId >= state.userCount)                  return ToyError::INVALID_INDEX;

    // Direct field access — no nested lock calls
    if (games[gameId].status != Status::ACTIVE)     return ToyError::GAME_INACTIVE;
    if (users[userId].status &Status::INACTIVE)     return ToyError::USER_INACTIVE;

    // Per player per game rate limit
    if (!IsEventAllowed(userId, gameId, timestamp)) return ToyError::RATE_LIMITED;

    // ── Match detection ───────────────────────────────────────────────────────
    // Must occur before the new event is written or added to eventsByGame
    // so that FindMatchableEvent only sees the opponent's existing events
    uint32_t matchId = FindMatchableEvent(gameId, userId, timestamp);

    uint32_t id = state.eventCount++;
    events[id]  = { gameId, userId, timestamp, EventStatus::UNMATCHED, EventStatus::PENDING, {} };

    // ── Link matched events ───────────────────────────────────────────────────
    if (matchId != EventStatus::UNMATCHED) {
        events[id].matchedEventId = matchId;
        events[matchId].matchedEventId = id;
        events[id].status = EventStatus::MATCHED;
        events[matchId].status = EventStatus::MATCHED;
    }

    eventsByGame[gameId].push_back(id);
	lastEventByPlayerGame[(static_cast<uint64_t>(userId) << 32) | gameId] = timestamp;

    eventIdOut = id;
    return ToyError::SUCCESS;
}

// ── Sweep ────────────────────────────────────────────────────────────────────
// Marks events that are still PENDING and unmatched, but past MATCH_SPAN_SECONDS
// old, as EXPIRED. Never touches a MATCHED event — matching is decided
// synchronously in AddEvent above, this only handles the "nothing happened"
// case. Returns the number of events swept, mainly for logging.
// Takes an explicit 'now' rather than reading the clock itself, so tests can
// simulate elapsed time without waiting for it.
int SweepExpiredEvents(int64_t now) {
    std::unique_lock lock(data_mutex);

    int count = 0;
    for (uint32_t i = 0; i < state.eventCount; ++i) {
        GameEvent& ev = events[i];
        if (ev.status == EventStatus::PENDING &&
            ev.matchedEventId == EventStatus::UNMATCHED &&
            (now - ev.timestamp) >= MATCH_SPAN_SECONDS) {
            ev.status = EventStatus::EXPIRED;
            ++count;
        }
    }
    return count;
}

// ── Helper ───────────────────────────────────────────────────────────────────

const char* ErrorString(int code) {
    switch (code) {
    case ToyError::SUCCESS:               return "SUCCESS";
    case ToyError::INSUFFICIENT_CAPACITY: return "INSUFFICIENT_CAPACITY";
    case ToyError::NAME_TOO_LONG:         return "NAME_TOO_LONG";
    case ToyError::DUPLICATE_KEY:         return "DUPLICATE_KEY";
    case ToyError::INVALID_INDEX:         return "INVALID_INDEX";
    case ToyError::GAME_LIMIT_REACHED:    return "GAME_LIMIT_REACHED";
    case ToyError::DUPLICATE_GAME:        return "DUPLICATE_GAME";
    case ToyError::GAME_INACTIVE:         return "GAME_INACTIVE";
    case ToyError::USER_INACTIVE:         return "USER_INACTIVE";
    case ToyError::RATE_LIMITED:          return "RATE_LIMITED";
    case ToyError::GAME_NOT_PENDING:      return "GAME_NOT_PENDING";
    case ToyError::NOT_INVITED:           return "NOT_INVITED";
    default:                              return "UNKNOWN";
    }
}

void PrintResult(std::string_view label, int result, uint32_t id) {
    if (result == ToyError::SUCCESS)
        printf("  %-32.*s -> ID: %u\n",
            (int)label.size(), label.data(), id);
    else
        printf("  %-32.*s -> ERROR: %s\n",
            (int)label.size(), label.data(), ErrorString(result));
}

#define LOG(msg) std::cout << msg << std::endl;
