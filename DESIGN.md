# ToY — Thinking of You
## Design Document

---

## Concept

ToY is a game about human connection. Two players each record moments
when they are thinking of the other person. If both players record an
event within the same one-hour window, they receive a score based on
how close in time their events occurred. The closer the timing, the
higher the score.

The game makes no claim about what causes a high-scoring event. It
simply measures temporal proximity between two independent acts of
thinking about another person. Participants can hold whatever
interpretation they prefer — coincidence or something else — without
the game contradicting them.

**Cheating** — agreeing with a partner to submit events simultaneously
— is functionally identical to deliberately thinking of someone at an
agreed moment. The act of honoring that agreement is the behavior the
game is trying to encourage. The system is cheat-resistant in a deeper
sense than most games because the exploit is the intended experience.

**Scoring formula:**
```
score = MATCH_SPAN_SECONDS - |event1.timestamp - event2.timestamp|

Maximum score: 3599  (events 1 second apart)
Minimum score: 0     (events exactly 1 hour apart)
No match:      events more than 1 hour apart
```

Both players receive the same score for a matched pair.

---

## Gameplay Rules

- A player may submit at most one event per game per hour
- The rate limit is per player per game — one player's event does not
  block the other player from submitting
- After a player submits an event, their partner is not notified unless:
  - The partner submits an event within the hour (match — both notified
    immediately with the score)
  - The one-hour window expires without a match (partner notified of
    the original event at expiry)
- After expiry notification, the partner has the opportunity to respond,
  starting a new hour window with roles reversed
- A player is limited to 10 active games simultaneously
- Games are never deleted — data accumulates and is archived in future
  maintenance events
- Players are never deleted — a player may be marked inactive or
  user-deleted but their record and name remain reserved

---

## Architecture

```
Browser (PWA)
    │
    ├── Static files (HTML, CSS, JS, manifest, service worker)
    │       Development: served directly by Crow
    │       Production:  served by Nginx from /var/www/toy/
    │
    └── /api/* requests
            │
            Nginx (production, TLS termination, reverse proxy)
                │
                Crow HTTP server (port 8088)
                    │
                    Application logic (toy.cpp)
                        │
                        Memory mapped data file (toy.dat)
```

### Deployment Target

- **Platform:** Linux on AWS EC2 (t3.micro initially, vertically
  scalable)
- **Web server:** Nginx (TLS, static files, reverse proxy to Crow)
- **TLS:** Let's Encrypt via Certbot (auto-renewing)
- **Process management:** systemd service
- **DNS:** Namecheap domain, A record pointed at AWS Elastic IP
- **Single binary deployment:** one process handles the full application

### Build System

CMake with FetchContent for dependencies. Builds on both Windows
(MSVC) and Linux (GCC/Clang) from the same CMakeLists.txt.

**Dependencies fetched at configure time:**
- Crow v1.2.0 — HTTP server
- Asio 1.28.0 — networking (required by Crow)
- nlohmann/json v3.11.3 — JSON serialization

**Targets:**
- `toy-core` — static library, production build, no static storage
- `toy-core-test` — static library, test build, `TOY_STATIC_STORAGE`
  defined so `Initialize()` uses static arrays instead of mapped memory
- `toy-server` — web service binary, links against `toy-core`
- `toy-tests` — unit test binary, links against `toy-core-test`

---

## Data Layer

### Design Principles

- All persistent data lives in a single memory mapped file (`toy.dat`)
- Three flat arrays of fixed-size structs form the tables
- Array index is the implicit primary key (player ID, game ID, event ID)
- All keys are `uint32_t` — 32 bits is sufficient for projected scale,
  smaller than `size_t` on 64-bit platforms, and produces a stable
  file format across process bitness
- No pointers in any persisted struct — safe to reload across restarts
- Tables are the authoritative source of truth
- Indexes are derived, in-memory only, rebuilt from tables on startup
- No deletions — the lifecycle is append-only with a future archiving
  path for maintenance

### File Layout

```
toy.dat
┌─────────────────────────────────────┐
│ TableState                          │  userCount, gameCount, eventCount
├─────────────────────────────────────┤
│ User[USER_CAPACITY]                 │  32 bytes each
├─────────────────────────────────────┤
│ Game[GAME_CAPACITY]                 │  16 bytes each
├─────────────────────────────────────┤
│ GameEvent[EVENT_CAPACITY]           │  32 bytes each
└─────────────────────────────────────┘
```

Layout offsets are computed at compile time as `constexpr` values.
Table pointers are set to the correct offsets into the mapped memory
at startup.

### Struct Definitions

```cpp
struct TableState {
    uint32_t userCount;
    uint32_t gameCount;
    uint32_t eventCount;
};

struct alignas(32) User {
    std::array<char, 24> name;   // null terminated, max NAME_LEN - 1 chars
    uint32_t             status; // bitmask — see Status namespace
    uint64_t             token;  // device authentication token
};

struct alignas(16) Game {
    uint32_t player1Id;
    uint32_t player2Id;
    uint32_t status;             // bitmask — see Status namespace
    uint32_t padding;
};

struct alignas(32) GameEvent {
    uint32_t gameId;
    uint32_t userId;
    int64_t  timestamp;          // Unix timestamp, set server-side
    uint32_t matchedEventId;     // UNMATCHED (UINT32_MAX) if no match
    uint32_t status;             // bitmask — see EventStatus namespace
    uint8_t  padding[8];
};
```

### Status Flags

```cpp
namespace Status {
    static constexpr uint32_t ACTIVE        = 0x00000000;
    static constexpr uint32_t INACTIVE      = 0x00000001;
    static constexpr uint32_t AT_GAME_LIMIT = 0x00000002;
    static constexpr uint32_t USER_DELETED  = 0x00000004;
    static constexpr uint32_t ARCHIVED      = 0x00000008;
    static constexpr uint32_t SUSPENDED     = 0x00000010;
}

namespace EventStatus {
    static constexpr uint32_t PENDING  = 0x00000000;
    static constexpr uint32_t MATCHED  = 0x00000001;
    static constexpr uint32_t EXPIRED  = 0x00000002;
    static constexpr uint32_t NOTIFIED = 0x00000004;
}
```

### Constants

```cpp
static constexpr uint32_t USER_CAPACITY       = 100;
static constexpr uint32_t GAME_CAPACITY       = 200;
static constexpr uint32_t EVENT_CAPACITY      = 10000;
static constexpr uint32_t NAME_LEN            = 24;
static constexpr uint32_t MAX_GAMES_PER_PLAYER= 10;
static constexpr int64_t  MATCH_SPAN_SECONDS  = 3600;
static constexpr uint32_t UNMATCHED           = UINT32_MAX;
```

Capacity constants are intentionally small for development. Scale by
increasing these constants, recompiling, and redeploying — the memory
mapped file grows automatically to the new capacity on next startup.

---

## Indexes

All indexes are `std::unordered_map` instances held in memory only.
They are cleared and rebuilt from the tables every time the data file
is opened. They are never written to disk.

```cpp
// Name → user ID
// Prevents duplicate names, enables lookup by name
// Key is std::string_view pointing into users[i].name — zero copy
// Valid because names are never modified after insertion
std::unordered_map<std::string_view, uint32_t>
    usersByName;

// Token → user ID
// Device authentication — token generated at registration,
// stored in User struct, persisted in memory mapped file
std::unordered_map<uint64_t, uint32_t>
    usersByToken;

// User ID → [game IDs]
// All games a player is involved in
// Used for game limit enforcement and GetGamesForPlayer queries
std::unordered_map<uint32_t, std::vector<uint32_t>>
    gamesByUser;

// Canonical player pair → game ID
// Prevents duplicate games between the same two players
// Key always stores lower ID first regardless of argument order
std::unordered_map<PlayerPair, uint32_t, PlayerPairHash>
    gamesByPair;

// Game ID → [event IDs]
// All events in a game in insertion (time) order
// Used for match detection, event history queries, notification worker
std::unordered_map<uint32_t, std::vector<uint32_t>>
    eventsByGame;

// Player ID + Game ID → last event timestamp
// Enforces per-player-per-game rate limiting
// Key combines playerId (high 32 bits) and gameId (low 32 bits)
// into a single uint64_t via PlayerGameKey()
std::unordered_map<uint64_t, int64_t>
    lastEventByPlayerGame;
```

### PlayerPair

```cpp
struct PlayerPair {
    uint32_t lowId;   // always min(player1, player2)
    uint32_t highId;  // always max(player1, player2)

    PlayerPair(uint32_t a, uint32_t b)
        : lowId(a < b ? a : b), highId(a < b ? b : a) {}

    bool operator==(const PlayerPair& other) const {
        return lowId == other.lowId && highId == other.highId;
    }
};
```

Canonical ordering means `PlayerPair(0, 1)` and `PlayerPair(1, 0)`
produce identical keys — the pair index is always order-independent.

### Index Rebuild

On startup with an existing data file, all six indexes are rebuilt
by a single forward pass through each table. This is the crash
recovery path — if the process terminates unexpectedly, the tables
on disk remain authoritative and the indexes are reconstructed
correctly on next startup.

```
RebuildIndexes() passes:
  users[]  → usersByName, usersByToken
  games[]  → gamesByUser, gamesByPair
  events[] → eventsByGame, lastEventByPlayerGame
```

---

## Core Functions

### AddUser

```
Validates: capacity, name length, duplicate name
Writes:    users[index] — name, status, token
Updates:   usersByName, usersByToken
Advances:  state.userCount
Returns:   ToyError code, userIdOut by reference
```

The `string_view` key in `usersByName` points directly into
`users[index].name` — zero copy. Safe because names are never
modified after insertion and users are never deleted.

Token is generated inside `AddUser` using `std::random_device` and
`std::mt19937_64`. The token is stored in the User struct (persisted)
and in `usersByToken` (rebuilt on startup).

### AddGame

```
Validates: capacity, valid player IDs, not same player,
           both players active, neither at game limit,
           pair does not already have a game
Writes:    games[id] — player1Id, player2Id, status
Updates:   gamesByUser (both players), gamesByPair
Sets:      AT_GAME_LIMIT flag if either player reaches limit
Advances:  state.gameCount
Returns:   ToyError code, gameIdOut by reference
```

Status checks inside `AddGame` access `users[].status` directly
rather than calling `IsUserActive()` — avoids deadlock since
`AddGame` already holds the `unique_lock`.

### AddEvent

```
Validates: capacity, valid game ID, valid user ID,
           game active, user active, rate limit not exceeded
Detects:   match via FindMatchableEvent (called BEFORE
           adding new event to eventsByGame)
Writes:    events[id] — all fields including matchedEventId
           If match: links both events via matchedEventId,
           sets MATCHED status on both
Updates:   eventsByGame, lastEventByPlayerGame
Advances:  state.eventCount
Returns:   ToyError code, eventIdOut by reference
```

**Critical ordering inside AddEvent:**
1. Validate
2. Call `FindMatchableEvent` — scans `eventsByGame` for opponent's
   unmatched events within the hour window
3. Write new event record
4. If match found: link both events, set MATCHED status
5. Update `eventsByGame` and `lastEventByPlayerGame`

The new event must not be in `eventsByGame` when `FindMatchableEvent`
runs — otherwise it could find itself.

### FindMatchableEvent

Scans `eventsByGame[gameId]` in reverse (most recent first) looking
for an unmatched event from the other player within `MATCH_SPAN_SECONDS`.
Stops early when an event outside the window is reached — events are
time-ordered so nothing earlier can match.

---

## Error Codes

All application error codes are in the `ToyError` namespace to avoid
collision with Windows error codes (`ERROR_SUCCESS`, `ERROR_INVALID_INDEX`
are defined in `Windows.h`).

```cpp
namespace ToyError {
    static constexpr int SUCCESS               =  0;
    static constexpr int INSUFFICIENT_CAPACITY = -1;
    static constexpr int NAME_TOO_LONG         = -2;
    static constexpr int DUPLICATE_KEY         = -3;
    static constexpr int GAME_INACTIVE         = -4;
    static constexpr int USER_INACTIVE         = -5;
    static constexpr int RATE_LIMITED          = -6;
    static constexpr int INVALID_INDEX         = -7;
    static constexpr int GAME_LIMIT_REACHED    = -8;
    static constexpr int DUPLICATE_GAME        = -9;
}
```

---

## Thread Safety

All shared state is protected by a single `std::shared_mutex`:

```cpp
extern std::shared_mutex data_mutex;
```

- **Write operations** (`AddUser`, `AddGame`, `AddEvent`, status
  setters) acquire `std::unique_lock<std::shared_mutex>` — exclusive
  access, blocks all readers and writers
- **Read operations** (`GetGamesForPlayer`, `GetEventsForGame`, status
  queries from external callers) acquire
  `std::shared_lock<std::shared_mutex>` — shared access, multiple
  readers allowed simultaneously

**Deadlock prevention:** Functions that hold a lock must not call other
functions that acquire the same lock. Inside `AddUser`, `AddGame`, and
`AddEvent`, status is checked by direct field access (`users[id].status`)
rather than by calling `IsUserActive()` or `IsGameActive()` which would
attempt to acquire a `shared_lock` on an already exclusively locked mutex.

`IsEventAllowed()` has no lock of its own — it is only called from
within `AddEvent` which already holds the lock.

---

## Authentication

Device-bound token authentication — no passwords.

1. Player registers with a name — `POST /api/users`
2. Server generates a 64-bit cryptographically random token
3. Token is stored in `users[id].token` (persisted in memory mapped file)
4. Token is returned to the PWA as a 16-character hex string
5. PWA stores the token (localStorage or service worker cache)
6. Every subsequent request includes the token as a Bearer header:
   `Authorization: Bearer a1b2c3d4e5f6a7b8`
7. `AuthMiddleware` validates the token on every request except
   registration and health check

The device is the credential. Losing the device means losing access —
no account recovery mechanism is currently implemented. The user
creates a new account with the same or a different name.

---

## HTTP API

All responses are JSON with `Content-Type: application/json`.
All timestamps are Unix epoch integers set server-side — clients
cannot submit timestamps, which prevents score manipulation.

### Unauthenticated Endpoints

```
GET  /api/health
     Returns: { status, userCount, gameCount, eventCount }

POST /api/users
     Body:    { "name": "Alice" }
     Returns: { "id": 0, "name": "Alice", "token": "a1b2c3d4..." }
```

### Authenticated Endpoints (Bearer token required)

```
GET  /api/users/{id}
     Returns: { id, name, status, gameCount }

GET  /api/users/search?name=Alice
     Returns: { id, name }

POST /api/games
     Body:    { "partnerId": 1 }
     Returns: { id, player1, player2, status }

GET  /api/games
     Returns: [ { id, player1, player2, status }, ... ]

POST /api/games/{id}/events
     No body — timestamp set server-side
     Returns: { id, gameId, matched }
              or on match: { id, gameId, matched: true,
                             matchedEventId, score }

GET  /api/games/{id}/events
     Returns: [ { id, userId, ts, status, matched,
                  score?, matchedEventId? }, ... ]

GET  /api/games/{id}/view
     Returns: plain text fixed-width game display
     Content-Type: text/plain; charset=utf-8

GET  /api/notifications
     Returns: [ pending notifications ] (polled by PWA)
```

### HTTP Status Codes

```
200  OK — successful read
201  Created — successful write (AddUser, AddGame, AddEvent)
400  Bad Request — missing or malformed request body
401  Unauthorized — missing, invalid, or expired token
403  Forbidden — access denied, user inactive, game limit reached
404  Not Found — resource does not exist
409  Conflict — duplicate name or duplicate game
422  Unprocessable — name too long
429  Too Many Requests — rate limited (one event per hour)
500  Internal Server Error
507  Insufficient Storage — server at capacity
```

---

## Game View — Text Rendering

The game view is rendered as plain text by the backend and displayed
in a `<pre>` element on the frontend. This keeps the frontend simple
and puts the formatting logic where the data already lives.

```
         Alice       Bob

─── Apr 16 ──────────────────
   12:05a  →
✓  01:10p  ↔  01:15p   +3544
             ←  12:01p

─── Apr 15 ──────────────────
✓  08:22a  ↔  09:47a   +2135
✓  03:15p  ↔  03:16p   +3599
   11:58p  →
```

**Symbols:**
- `→` — left player sent an event, awaiting response
- `←` — right player sent an event, awaiting response
- `↔` — matched pair
- `✓` — match indicator
- `+N` — score for the matched pair

Matched pairs are rendered once (from the earlier event's perspective).
The score is derived on the fly from the two timestamps — it is never
stored separately.

---

## Notification Design

### Two notification paths

**Immediate match notification** — when `AddEvent` detects a match,
both players are notified immediately with the score. This is handled
synchronously inside `AddEvent`.

**Expiry notification** — when the one-hour window expires without a
match, the other player is notified of the original event. This is
handled by a background notification worker thread (not yet
implemented — `GetPendingNotifications` returns an empty array as a
stub).

### Notification delivery

The PWA polls `GET /api/notifications` every 30 seconds. This is
the simplest delivery mechanism and sufficient for an application
where the minimum meaningful interval is one hour.

Web Push (via the browser Push API and service worker) is a planned
enhancement — it allows notifications to arrive even when the PWA
is not open, which is more aligned with the spirit of the game.

---

## PWA Frontend

### Philosophy

The same design principles that guide the backend apply to the
frontend: simplest correct solution, nothing unnecessary, everything
for a clear reason.

- Vanilla HTML, CSS, and JavaScript — no framework
- Single `index.html` with multiple views managed by JS state
- CSS custom properties for consistent theming
- Fixed-width font (`<pre>` element) for the game view
- Three JavaScript responsibilities: API calls, view management,
  notification polling

### File Structure

```
www/
├── index.html          — single page, all views
├── style.css           — all styles, CSS custom properties
├── app.js              — API layer, view management, state
├── manifest.json       — PWA identity, icons, display mode
└── service-worker.js   — caching, offline support, push notifications
```

### Static File Serving

**Development (Windows):** Crow serves files from `www/` relative to
the working directory. Routes defined for `/`, `/style.css`, `/app.js`,
`/manifest.json`, `/service-worker.js`.

**Production (Linux):** Nginx serves files from `/var/www/toy/` with
compression and caching headers. Crow only handles `/api/*` requests
forwarded by Nginx.

---

## Memory Mapped File — Cross-Platform

```cpp
class MappedFile {
public:
    bool  open(const char* path, size_t size, bool& isNew);
    void  flush();
    void  close();
    void* data()   const;
    size_t size()  const;
    bool  isOpen() const;
};
```

`isNew` is set to `true` when the file is created for the first time.
The caller uses this to decide whether to zero-initialize the tables
(first run) or rebuild indexes from existing data (subsequent runs).

**Windows:** `CreateFileA` / `CreateFileMapping` / `MapViewOfFile`
**Linux:** `open` / `ftruncate` / `mmap`

**Flush:**
**Windows:** `FlushViewOfFile` + `FlushFileBuffers`
**Linux:** `msync(MS_SYNC)`

HANDLE types are stored as `void*` in the header to avoid including
`Windows.h` from `toy-storage.h`. Cast to `HANDLE` inside
`toy-storage.cpp` after `Windows.h` is included.

---

## Configuration

Runtime configuration is loaded from `settings.json` alongside the
executable. The file is optional — defaults are used if absent or
unparsable.

```json
{
    "port": 8088,
    "threads": 2
}
```

Planned additions:
```json
{
    "port": 8088,
    "threads": 2,
    "dataFile": "toy.dat",
    "logLevel": "info",
    "matchWindowSeconds": 3600,
    "maxGamesPerPlayer": 10
}
```

Moving game parameters into configuration allows tuning without
recompilation, which is useful during early deployment when observing
actual player behavior.

---

## Security

### Rate Limiting (layered defence)

```
Layer 1  Namecheap / CDN    DDoS protection at network level
Layer 2  AWS WAF            HTTP-level rate limiting per IP (optional)
Layer 3  Nginx              Per-IP rate limiting before requests reach Crow
Layer 4  Crow application   In-process per-IP limiter for development
Layer 5  Game rules         One event per player per game per hour
```

### Attack Surface

The only unauthenticated endpoints with side effects are:
- `POST /api/users` — registration

All other endpoints require a valid Bearer token. The worst outcome
of a compromised session is noise in the event table — no financial
data, no sensitive personal information, and no mechanism to affect
other users' data.

User data stored: name only (chosen by the user, may be pseudonymous).

---

## Scaling Path

The design scales vertically without code changes:

```
t3.micro   →  t3.small  →  t3.medium  →  t3.large  →  t3.xlarge
Increase USER_CAPACITY, GAME_CAPACITY, EVENT_CAPACITY constants
Recompile and redeploy — memory mapped file grows automatically
```

The `uint32_t` key type supports up to ~4.29 billion records per
table — well beyond the practical limits of a single-instance
deployment.

Migration between instances is a file copy:
```bash
sudo systemctl stop toy-server
rsync toy.dat user@new-instance:/opt/toy/
sudo systemctl start toy-server
```

Indexes rebuild automatically from the file on startup.

---

## Known Limitations and Future Work

| Item | Status |
|---|---|
| Notification worker thread | Stub — `GetPendingNotifications` returns `[]` |
| Web Push notifications | Planned — requires VAPID integration |
| Account recovery | Not implemented — new account on device loss |
| Linux memory mapped file | Implemented, untested (Windows only so far) |
| PWA frontend | In progress |
| TLS / HTTPS | Planned via Let's Encrypt on Linux deployment |
| Nginx configuration | Planned for Linux deployment |
| AWS deployment | Planned |
| RenderGameView function | Designed, not yet integrated into routes |
| Archive / pruning path | Planned for future maintenance events |

---

## Project Name

**ToY** — Thinking of You

The name is immediately understood, emotionally positive, and carries
no baggage. The acronym adds lightness that signals participation
should feel playful rather than pressured — balancing emotional
sincerity with playfulness, which is exactly right for what the game
is about.
