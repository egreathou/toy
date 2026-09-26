
#include "toy.h"   // when the project is split into header and implementation
#include <cstdio>
#include <cstring>
#include <cstdint>

// ── Minimal test framework ────────────────────────────────────────────────────

#define state (*statePtr)

static uint32_t testsRun = 0;
static uint32_t testsPassed = 0;
static uint32_t testsFailed = 0;

#define CHECK(condition, description)                              \
    do {                                                           \
        ++testsRun;                                                \
        if (condition) {                                           \
            ++testsPassed;                                         \
            printf("  PASS  %s\n", description);                   \
        } else {                                                   \
            ++testsFailed;                                         \
            printf("  FAIL  %s  [%s:%d]\n",                        \
                description, __FILE__, __LINE__);                  \
        }                                                          \
    } while(0)

#define CHECK_EQ(actual, expected, description)                    \
    do {                                                           \
        ++testsRun;                                                \
        if ((actual) == (expected)) {                              \
            ++testsPassed;                                         \
            printf("  PASS  %s\n", description);                   \
        } else {                                                   \
            ++testsFailed;                                         \
            printf("  FAIL  %s  expected=%d actual=%d [%s:%d]\n",  \
                description, (int)(expected), (int)(actual),       \
                __FILE__, __LINE__);                               \
        }                                                          \
    } while(0)

#define TEST(name)    static void name()
#define RUN(name)     do { printf("\n" #name "\n"); name(); } while(0)

static void PrintSummary() {
    printf("\n----------------------------------------\n");
    printf("  %u / %u tests passed", testsPassed, testsRun);
    if (testsFailed > 0)
        printf("  (%u FAILED)", testsFailed);
    printf("\n----------------------------------------\n");
}

// ── Reset helpers ─────────────────────────────────────────────────────────────

static void ResetAll() {
    Initialize();  // memsets tables, clears and reserves indexes
}

// ── Test suites ───────────────────────────────────────────────────────────────

TEST(Test_AddUser_Basic) {
    ResetAll();
    uint32_t id;

    CHECK_EQ(AddUser("Eric", id), ToyError::SUCCESS, "add Eric");
    CHECK_EQ(id, 0, "Eric gets ID 0");
    CHECK_EQ(AddUser("Dana", id), ToyError::SUCCESS, "add Dana");
    CHECK_EQ(id, 1, "Dana gets ID 1");
    CHECK_EQ(AddUser("Kerry", id), ToyError::SUCCESS, "add Kerry");
    CHECK_EQ(AddUser("Aaron", id), ToyError::SUCCESS, "add Aaron");
    CHECK_EQ(state.userCount, 4u, "userCount is 4");
}

TEST(Test_AddUser_Duplicate) {
    ResetAll();
    uint32_t id;

    AddUser("Eric", id);
    CHECK_EQ(AddUser("Eric", id), ToyError::DUPLICATE_KEY, "reject duplicate name");
    CHECK_EQ(state.userCount, 1u, "userCount unchanged after duplicate");
}

TEST(Test_AddUser_NameTooLong) {
    ResetAll();
    uint32_t id;

    // Exactly NAME_LEN chars — should be rejected (>= NAME_LEN)
    CHECK_EQ(AddUser("ABCDEFGHIJKLMNOPQRSTUVWXYZ12", id),
        ToyError::NAME_TOO_LONG, "reject name of NAME_LEN chars");

    // NAME_LEN - 1 chars — should be accepted
    CHECK_EQ(AddUser("ABCDEFGHIJKLMNOPQRSTUVWXYZ1", id),
        ToyError::SUCCESS, "accept name of NAME_LEN-1 chars");
}

TEST(Test_AddUser_Capacity) {
    ResetAll();
    uint32_t id;

    // Fill to capacity
    char name[NAME_LEN];
    for (uint32_t i = 0; i < USER_CAPACITY; ++i) {
        snprintf(name, NAME_LEN, "User%u", i);
        AddUser(name, id);
    }

    CHECK_EQ(AddUser("OneMore", id),
        ToyError::INSUFFICIENT_CAPACITY, "reject when at user capacity");
}

TEST(Test_UsersByName_Index) {
    ResetAll();
    uint32_t id;

    AddUser("Eric", id);
    AddUser("Dana", id);

    auto it = usersByName.find("Eric");
    CHECK(it != usersByName.end(), "Eric found in index");
    CHECK_EQ(it->second, 0u, "Eric has ID 0");

    // Verify string_view key points into stable table storage
    CHECK(it->first.data() == users[0].name.data(), "key points into users table");

    CHECK(usersByName.find("Unknown") == usersByName.end(), "Unknown not in index");
}

TEST(Test_AddGame_Basic) {
    ResetAll();
    uint32_t idEric, idDana, gameId;

    AddUser("Eric", idEric);
    AddUser("Dana", idDana);

    CHECK_EQ(AddGame(idEric, idDana, gameId), ToyError::SUCCESS, "add Eric vs Dana");
    CHECK_EQ(gameId, 0u, "first game gets ID 0");
    CHECK_EQ(state.gameCount, 1u, "gameCount is 1");

    // Verify both players appear in gamesByUser
    CHECK_EQ(gamesByUser[idEric].size(), 1u, "Eric has 1 game");
    CHECK_EQ(gamesByUser[idDana].size(), 1u, "Dana has 1 game");
    CHECK_EQ(gamesByUser[idEric][0], 0u, "Eric's game is ID 0");
}

TEST(Test_AddGame_Duplicate) {
    ResetAll();
    uint32_t idEric, idDana, gameId;

    AddUser("Eric", idEric);
    AddUser("Dana", idDana);
    AddGame(idEric, idDana, gameId);

    CHECK_EQ(AddGame(idEric, idDana, gameId),
        ToyError::DUPLICATE_GAME, "reject duplicate game same order");
    CHECK_EQ(AddGame(idDana, idEric, gameId),
        ToyError::DUPLICATE_GAME, "reject duplicate game reversed order");
}

TEST(Test_AddGame_SamePlayer) {
    ResetAll();
    uint32_t idEric, gameId;

    AddUser("Eric", idEric);
    CHECK_EQ(AddGame(idEric, idEric, gameId),
        ToyError::INVALID_INDEX, "reject game with same player twice");
}

TEST(Test_AddGame_GameLimit) {
    ResetAll();
    uint32_t ids[MAX_GAMES_PER_PLAYER + 2];
    uint32_t gameId;
    char name[NAME_LEN];

    // Create enough players for MAX_GAMES_PER_PLAYER + 1 opponents
    for (uint32_t i = 0; i < MAX_GAMES_PER_PLAYER + 2; ++i) {
        snprintf(name, NAME_LEN, "Player%u", i);
        AddUser(name, ids[i]);
    }

    // Player 0 plays MAX_GAMES_PER_PLAYER games
    for (uint32_t i = 1; i <= MAX_GAMES_PER_PLAYER; ++i)
        AddGame(ids[0], ids[i], gameId);

    CHECK_EQ(AddGame(ids[0], ids[MAX_GAMES_PER_PLAYER + 1], gameId),
        ToyError::GAME_LIMIT_REACHED, "reject game when player at limit");
    CHECK(users[ids[0]].status & ::Status::AT_GAME_LIMIT, "AT_GAME_LIMIT flag set");
}

TEST(Test_AddGame_StartsPending) {
    ResetAll();
    uint32_t idEric, idDana, gameId;

    AddUser("Eric", idEric);
    AddUser("Dana", idDana);
    AddGame(idEric, idDana, gameId);

    CHECK(games[gameId].status & Status::PENDING_INVITE, "new game starts pending");
    CHECK(!IsGameActive(gameId), "pending game is not active");
}

TEST(Test_AcceptGame_Basic) {
    ResetAll();
    uint32_t idEric, idDana, gameId;

    AddUser("Eric", idEric);
    AddUser("Dana", idDana);
    AddGame(idEric, idDana, gameId);

    CHECK_EQ(AcceptGame(gameId, idDana), ToyError::SUCCESS, "invited player accepts");
    CHECK(IsGameActive(gameId), "game is active after acceptance");
}

TEST(Test_AcceptGame_WrongPlayer) {
    ResetAll();
    uint32_t idEric, idDana, gameId;

    AddUser("Eric", idEric);
    AddUser("Dana", idDana);
    AddGame(idEric, idDana, gameId);

    // Player1 (the inviter) cannot accept their own invite
    CHECK_EQ(AcceptGame(gameId, idEric), ToyError::NOT_INVITED, "inviter cannot accept own invite");
    CHECK(!IsGameActive(gameId), "game remains pending");
}

TEST(Test_AcceptGame_AlreadyAccepted) {
    ResetAll();
    uint32_t idEric, idDana, gameId;

    AddUser("Eric", idEric);
    AddUser("Dana", idDana);
    AddGame(idEric, idDana, gameId);
    AcceptGame(gameId, idDana);

    CHECK_EQ(AcceptGame(gameId, idDana), ToyError::GAME_NOT_PENDING, "reject accepting twice");
}

TEST(Test_AcceptGame_InvalidGame) {
    ResetAll();
    uint32_t idEric;

    AddUser("Eric", idEric);
    CHECK_EQ(AcceptGame(999, idEric), ToyError::INVALID_INDEX, "reject nonexistent game");
}

TEST(Test_AddEvent_BlockedWhilePending) {
    ResetAll();
    uint32_t idEric, idDana, gameId, eventId;

    AddUser("Eric", idEric);
    AddUser("Dana", idDana);
    AddGame(idEric, idDana, gameId);

    CHECK_EQ(AddEvent(gameId, idEric, 1000000, eventId),
        ToyError::GAME_INACTIVE, "event rejected before invite is accepted");

    AcceptGame(gameId, idDana);

    CHECK_EQ(AddEvent(gameId, idEric, 1000000, eventId),
        ToyError::SUCCESS, "event accepted once game is active");
}

TEST(Test_AddEvent_Basic) {
    ResetAll();
    uint32_t idEric, idDana, gameId, eventId;
    int64_t  t1 = 1000000;

    AddUser("Eric", idEric);
    AddUser("Dana", idDana);
    AddGame(idEric, idDana, gameId);
    AcceptGame(gameId, idDana);

    CHECK_EQ(AddEvent(gameId, idEric, t1, eventId),
        ToyError::SUCCESS, "Eric sends first event");
    CHECK_EQ(eventId, 0u, "first event gets ID 0");
    CHECK_EQ(state.eventCount, 1u, "eventCount is 1");
    CHECK_EQ(eventsByGame[gameId].size(), 1u, "game has 1 event");
}

TEST(Test_AddEvent_RateLimit) {
    ResetAll();
    uint32_t idEric, idDana, gameId, eventId;
    int64_t  t1 = 1000000;

    AddUser("Eric", idEric);
    AddUser("Dana", idDana);
    AddGame(idEric, idDana, gameId);
    AcceptGame(gameId, idDana);

    AddEvent(gameId, idEric, t1, eventId);

    // Same player tries again within the hour
    CHECK_EQ(AddEvent(gameId, idEric, t1 + 1, eventId),
        ToyError::RATE_LIMITED, "reject second event within hour");

    // Same player tries exactly at the boundary — still blocked
    CHECK_EQ(AddEvent(gameId, idEric, t1 + MATCH_SPAN_SECONDS - 1, eventId),
        ToyError::RATE_LIMITED, "reject event at hour boundary");

    // Same player tries after the hour — allowed
    CHECK_EQ(AddEvent(gameId, idEric, t1 + MATCH_SPAN_SECONDS, eventId),
        ToyError::SUCCESS, "accept event after full hour");
}

TEST(Test_AddEvent_RateLimit_PerPlayer) {
    ResetAll();
    uint32_t idEric, idDana, gameId, eventId;
    int64_t  t1 = 1000000;

    AddUser("Eric", idEric);
    AddUser("Dana", idDana);
    AddGame(idEric, idDana, gameId);
    AcceptGame(gameId, idDana);

    AddEvent(gameId, idEric, t1, eventId);

    // Other player is NOT rate limited by Eric's event
    CHECK_EQ(AddEvent(gameId, idDana, t1 + 1, eventId),
        ToyError::SUCCESS, "Dana can send event after Eric");
}

TEST(Test_AddEvent_Match) {
    ResetAll();
    uint32_t idEric, idDana, gameId, eventId;
    int64_t  t1 = 1000000;
    int64_t  t2 = t1 + 1;  // 1 second apart — score should be 3599

    AddUser("Eric", idEric);
    AddUser("Dana", idDana);
    AddGame(idEric, idDana, gameId);
    AcceptGame(gameId, idDana);

    AddEvent(gameId, idEric, t1, eventId);
    uint32_t ericEventId = eventId;

    CHECK_EQ(AddEvent(gameId, idDana, t2, eventId),
        ToyError::SUCCESS, "Dana responds within hour");

    // Verify both events are linked
    CHECK_EQ(events[eventId].matchedEventId, ericEventId, "Dana's event linked to Eric's");
    CHECK_EQ(events[ericEventId].matchedEventId, eventId, "Eric's event linked to Dana's");

    // Verify score
    uint32_t score = static_cast<uint32_t>(
        MATCH_SPAN_SECONDS - std::abs(t2 - t1));
    CHECK_EQ(score, 3599u, "score is 3599 for 1 second apart");
}

TEST(Test_AddEvent_NoMatch) {
    ResetAll();
    uint32_t idEric, idDana, gameId, eventId;
    int64_t  t1 = 1000000;
    int64_t  t2 = t1 + MATCH_SPAN_SECONDS + 1;  // just outside the window

    AddUser("Eric", idEric);
    AddUser("Dana", idDana);
    AddGame(idEric, idDana, gameId);
    AcceptGame(gameId, idDana);

    AddEvent(gameId, idEric, t1, eventId);
    uint32_t ericEventId = eventId;

    AddEvent(gameId, idDana, t2, eventId);

    // Events should not be linked
    CHECK_EQ(events[ericEventId].matchedEventId, EventStatus::UNMATCHED, "Eric's event unmatched after window");
    CHECK_EQ(events[eventId].matchedEventId, EventStatus::UNMATCHED, "Dana's event unmatched");
}

TEST(Test_Sweep_ExpiresUnmatchedPastWindow) {
    ResetAll();
    uint32_t idEric, idDana, gameId, eventId;
    int64_t  t1 = 1000000;

    AddUser("Eric", idEric);
    AddUser("Dana", idDana);
    AddGame(idEric, idDana, gameId);
    AcceptGame(gameId, idDana);
    AddEvent(gameId, idEric, t1, eventId);

    CHECK_EQ(events[eventId].status, EventStatus::PENDING, "event starts pending");

    // One second short of the window — not expired yet
    int count = SweepExpiredEvents(t1 + MATCH_SPAN_SECONDS - 1);
    CHECK_EQ(count, 0, "sweep finds nothing just under the window");
    CHECK_EQ(events[eventId].status, EventStatus::PENDING, "still pending just under the window");

    // Exactly at the window — now expired
    count = SweepExpiredEvents(t1 + MATCH_SPAN_SECONDS);
    CHECK_EQ(count, 1, "sweep finds exactly one event");
    CHECK_EQ(events[eventId].status, EventStatus::EXPIRED, "event marked expired at the window");
}

TEST(Test_Sweep_LeavesMatchedEventsAlone) {
    ResetAll();
    uint32_t idEric, idDana, gameId, eventId;
    int64_t  t1 = 1000000;
    int64_t  t2 = t1 + 1; // matches

    AddUser("Eric", idEric);
    AddUser("Dana", idDana);
    AddGame(idEric, idDana, gameId);
    AcceptGame(gameId, idDana);
    AddEvent(gameId, idEric, t1, eventId);
    uint32_t ericEventId = eventId;
    AddEvent(gameId, idDana, t2, eventId);

    CHECK_EQ(events[ericEventId].status, EventStatus::MATCHED, "matched before sweep runs");

    // Sweep long after the window — matched events must not be touched
    SweepExpiredEvents(t1 + MATCH_SPAN_SECONDS * 10);
    CHECK_EQ(events[ericEventId].status, EventStatus::MATCHED, "still matched after sweep");
    CHECK_EQ(events[eventId].status, EventStatus::MATCHED, "partner's event still matched after sweep");
}

TEST(Test_Sweep_LeavesOpenEventsAlone) {
    ResetAll();
    uint32_t idEric, idDana, gameId, eventId;
    int64_t  t1 = 1000000;

    AddUser("Eric", idEric);
    AddUser("Dana", idDana);
    AddGame(idEric, idDana, gameId);
    AcceptGame(gameId, idDana);
    AddEvent(gameId, idEric, t1, eventId);

    // Sweep at a time well within the still-open window
    int count = SweepExpiredEvents(t1 + 30);
    CHECK_EQ(count, 0, "sweep finds nothing while still open");
    CHECK_EQ(events[eventId].status, EventStatus::PENDING, "still pending, well within the window");
}

TEST(Test_RebuildIndexes) {
    ResetAll();
    uint32_t idEric, idDana, gameId, eventId;

    AddUser("Eric", idEric);
    AddUser("Dana", idDana);
    AddGame(idEric, idDana, gameId);
    AcceptGame(gameId, idDana);
    AddEvent(gameId, idEric, 1000000, eventId);

    // Capture state before rebuild
    uint32_t savedUserCount  = state.userCount;
    uint32_t savedGameCount  = state.gameCount;
    //uint32_t savedEventCount = state.eventCount;

    // Wipe indexes and rebuild
    usersByName.clear();
    gamesByUser.clear();
    gamesByPair.clear();
    eventsByGame.clear();
    lastEventByPlayerGame.clear();
    RebuildIndexes();

    CHECK_EQ((uint32_t)usersByName.size(), savedUserCount, "usersByName rebuilt");
    CHECK_EQ((uint32_t)gamesByUser.size(), savedUserCount, "gamesByUser rebuilt");
    CHECK_EQ((uint32_t)eventsByGame.size(), savedGameCount, "eventsByGame rebuilt");

    // Verify string_view key still points into table after rebuild
    auto it = usersByName.find("Eric");
    CHECK(it != usersByName.end(), "Eric found after rebuild");
    CHECK(it->first.data() == users[0].name.data(), "key points into table after rebuild");

    // Verify duplicate game still rejected after rebuild
    CHECK_EQ(AddGame(idEric, idDana, gameId),
        ToyError::DUPLICATE_GAME, "duplicate game rejected after rebuild");

    // Verify rate limit still enforced after rebuild
    CHECK_EQ(AddEvent(gameId, idEric, 1000001, eventId),
        ToyError::RATE_LIMITED, "rate limit enforced after rebuild");
}

// ── Entry point ───────────────────────────────────────────────────────────────

int main() {

    RUN(Test_AddUser_Basic);
    RUN(Test_AddUser_Duplicate);
    RUN(Test_AddUser_NameTooLong);
    RUN(Test_AddUser_Capacity);
    RUN(Test_UsersByName_Index);
    RUN(Test_AddGame_Basic);
    RUN(Test_AddGame_Duplicate);
    RUN(Test_AddGame_SamePlayer);
    RUN(Test_AddGame_GameLimit);
    RUN(Test_AddGame_StartsPending);
    RUN(Test_AcceptGame_Basic);
    RUN(Test_AcceptGame_WrongPlayer);
    RUN(Test_AcceptGame_AlreadyAccepted);
    RUN(Test_AcceptGame_InvalidGame);
    RUN(Test_AddEvent_BlockedWhilePending);
    RUN(Test_AddEvent_Basic);
    RUN(Test_AddEvent_RateLimit);
    RUN(Test_AddEvent_RateLimit_PerPlayer);
    RUN(Test_AddEvent_Match);
    RUN(Test_AddEvent_NoMatch);
    RUN(Test_Sweep_ExpiresUnmatchedPastWindow);
    RUN(Test_Sweep_LeavesMatchedEventsAlone);
    RUN(Test_Sweep_LeavesOpenEventsAlone);
    RUN(Test_RebuildIndexes);

    PrintSummary();
    return testsFailed == 0 ? 0 : 1;
}