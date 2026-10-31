# ResilientPOS

**An offline-first Point-of-Sale engine in modern C++20 that keeps selling through network failures.**

![CI](https://github.com/kgiannopoulou/ResilientPOS/actions/workflows/ci.yml/badge.svg)

A till persists every sale locally *before* it touches the network, queues
whatever the server did not confirm, and a background worker drains the queue
with exponential backoff once connectivity returns. The server is idempotent
by transaction ID, so a retry after a lost response can never charge a
customer twice.

## Problem

A shop's tills cannot stop selling because the Wi-Fi dropped or the central
server is being redeployed. The obvious fix, "save it and send it later", hides
three hard problems:

1. **Losing sales.** If the till crashes between taking payment and talking to
   the server, the sale must still exist.
2. **Duplicating sales.** The worst network failure is the *ambiguous* one: the
   server stores the sale, then the response is lost. The till sees an error and
   retries. Without care, the server now has the sale twice.
3. **Hammering a dead server.** A hundred tills retrying in a tight loop turn a
   short outage into a long one.

ResilientPOS is built around solving exactly those three, and proving it with tests.

## Demo

Two programs, each with its own SQLite database:

```
./build/resilient-server [--mode normal|latency|random|timeout|drop-before|drop-after|reject] [port=8080] [database=data/server.db]
./build/resilient-pos    [database=data/pos.db] [server-host=127.0.0.1] [server-port=8080]
```

**Online sale** — server running normally:

```
========================================
              ResilientPOS
========================================

Network: ONLINE
Pending transactions: 0

1. New transaction
2. List transactions
3. Force synchronization
4. Network status
5. Exit

> 1

Product: Coffee
Unit price: €3.50
Quantity: 2

----------------------------------------
2 x Coffee @ €3.50
Total: €7.00
----------------------------------------

Processing transaction...

[LOCAL] Transaction persisted
[NETWORK] Sending...
[SERVER] Accepted

ID:     7371609a...
Status: SYNCED
```

**Offline sale** — server stopped:

```
[LOCAL] Transaction persisted
[NETWORK] Sending...
[NETWORK] Server unavailable

ID:     dfd39240...
Status: PENDING_SYNC

Transaction safely stored locally.
POS can continue operating.
```

**The ambiguous failure** — `resilient-server --mode drop-after` stores the
sale, then hangs up. The till keeps it `PENDING_SYNC`. Switch the server back
to normal and sync:

```
> 3

[SYNC] Starting synchronization...

[SYNC] 03ae7ba4... -> ALREADY_PROCESSED -> SYNCED (server had it; no duplicate)

1 transaction(s) synchronized.
0 transaction(s) pending.
```

Server log for the same sale — stored once, recognised on the resend:

```
[2026-10-31 11:03:44] [INFO] Transaction accepted id=03ae7ba4 amount=700
[2026-10-31 11:03:44] [WARN] Simulated drop after processing: COMMITTED, response dropped
[2026-10-31 11:03:59] [INFO] Duplicate transaction id=03ae7ba4 result=ALREADY_PROCESSED (not stored again)
```

A scripted 3–5 minute walkthrough is in [docs/DEMO.md](docs/DEMO.md).

## Architecture

```
                        ┌─────────────┐
                        │   POS CLI   │   apps/pos/main.cpp: wiring + terminal UI only
                        └──────┬──────┘
                               │
                               ▼
                    ┌────────────────────┐
                    │ TransactionService │   persist first, send second
                    └─────────┬──────────┘
                              │
                ┌─────────────┴────────────┐
                ▼                          ▼
      TransactionRepository          ServerClient  ◄── MonitoredServerClient (connectivity)
                │                          │
                ▼                          ▼
    SQLiteTransactionRepository     HttpServerClient   (Asio/Beast, JSON, timeouts)
                │                          │
          data/pos.db                      ▼   POST /transactions
                                    HttpTransactionServer + NetworkSimulationMode
                                           │
                                           ▼
                                  handleTransactionRequest()
                                           │
                                           ▼
                                  ProcessedTransactionStore ── data/server.db


                   ┌──────────────────┐
                   │    SyncEngine    │   std::jthread worker + RetryPolicy
                   └────────┬─────────┘
                            │
                 ┌──────────┴─────────┐
                 ▼                    ▼
       TransactionRepository     ServerClient
```

`main.cpp` contains no business logic: it wires objects together and draws the
screens. `TransactionService` owns creating sales, `SyncEngine` owns recovery
and synchronization, `TransactionRepository` owns persistence and
`ServerClient` owns communication. Business logic depends only on the two
interfaces, which is what lets the unit tests swap in `FakeTransactionRepository`
and `FakeServerClient`.

## Reliability Guarantees

1. **A transaction is persisted before network transmission.**
   `TransactionService` writes the row to SQLite (WAL, `synchronous=FULL`, so it
   is fsynced) and only then calls `send()`. If saving fails, nothing is sent and
   the sale is refused. *Test: `Service persists the transaction before sending it`.*
2. **Pending transactions survive application restarts.** The queue *is* the
   database. A sale caught mid-send by a crash is requeued at the next start.
   *Tests: `IntegrationTest: pending sales survive destroying the POS`,
   `... a sale cut off mid-send is resent after restart`.*
3. **Every transaction has a globally unique ID.** Each sale gets a random v4
   UUID on the till, so IDs never need the server and never collide across tills.
4. **Server processing is idempotent by transaction ID.** The ID is the
   server table's primary key, written with `INSERT ... ON CONFLICT(id) DO NOTHING`:
   one atomic statement, no check-then-insert race. A duplicate answers
   `ALREADY_PROCESSED` and never overwrites the stored amount.
5. **A lost response does not cause duplicate processing.** The till retries
   with the *same* ID; `ALREADY_PROCESSED` counts as success and the sale ends
   `SYNCED`. *Test: `IntegrationTest: LostResponseDoesNotCauseDuplicateTransaction`.*
6. **Synchronization retries use exponential backoff.** 1 s, 2 s, 4 s, 8 s,
   16 s, then 30 s. New sales during a backoff do not cut it short.
   *Tests: `Worker retries a failing server with exponential backoff`,
   `New sales during a backoff do not trigger a retry storm`.*

## Persist First, Send Second

```
create transaction
       │
       ▼
save to SQLite as CREATED    ◄── the sale is now safe; a crash here loses nothing
       │
       ▼
send to server
     /     \
 success   failure
    │         │
 SYNCED   PENDING_SYNC  ──► notify the sync worker; the till carries on
```

A network or server failure never fails a sale. Only a storage failure does,
because then the sale really is recorded nowhere.

## Idempotency

The till can never know whether a request that got no answer was processed.
So it doesn't try to know: it always resends the same transaction ID, and the
server makes resending harmless.

```
POST /transactions
{"transaction_id": "550e8400-e29b-41d4-a716-446655440000", "amount_cents": 700}

200 {"status": "ACCEPTED"}            first time this ID is seen
200 {"status": "ALREADY_PROCESSED"}   ID already stored, nothing written
400 {"status": "REJECTED", "error": "..."}   malformed request
500 {"status": "ERROR", "error": "..."}      storage failure
503 {"status": "ERROR", "error": "simulated outage"}   --mode reject
```

Both 200 answers mean "the server has it". `SendResult::successful()` is true
for both, so the till marks the sale `SYNCED` either way.

## Transaction State Machine

```
CREATED ──────────────────────────────► SYNCED      first send accepted
   │                                      ▲
   ▼                                      │
PENDING_SYNC ──► SYNCING ─────────────────┘         resend accepted
      ▲             │
      └─────────────┘                               resend failed
```

| Status | Meaning | Owned by |
|---|---|---|
| `CREATED` | on disk, first send in flight | `TransactionService` |
| `PENDING_SYNC` | waiting in the offline queue | nobody (claimable) |
| `SYNCING` | a resend is in flight | `SyncEngine` |
| `SYNCED` | the server holds it — final | — |

`canTransition(from, to)` allows exactly these five moves; anything else
(`SYNCED → CREATED`, `PENDING_SYNC → SYNCED`, ...) throws
`InvalidTransitionError`, enforced inside the repository. Two details matter:

- **Ownership by state.** The worker only takes `PENDING_SYNC` rows, via an atomic
  compare-and-set (`tryUpdateStatus(id, PENDING_SYNC, SYNCING)`). A live sale is
  `CREATED` while the service sends it, so the two threads can never send the
  same sale at once.
- **Crash recovery.** A `CREATED` or `SYNCING` row on disk at startup belongs to a
  run that died mid-send. `recoverInterrupted()` moves it back to `PENDING_SYNC`
  (both legal moves) and it is resent; idempotency makes that safe whether or not
  the server got the first copy.

## Offline Synchronization

`SyncEngine::syncPendingTransactions()` is one pass: load pending rows oldest
first, claim each one, send it, mark it `SYNCED` or return it to `PENDING_SYNC`.

- A **`NetworkError` ends the pass early.** If the server is unreachable for one
  sale it is unreachable for all of them, and each attempt can take seconds.
- A **`ServerError` is about that one request**, so the pass carries on.
- `ALREADY_PROCESSED` is success, counted separately in the `SyncReport`.

The CLI's *Force synchronization* runs a pass on the UI thread; passes are
serialized, so it can never overlap the background worker's.

## Retry Strategy

`RetryPolicy::nextDelay(attempt)` = `min(1 s × 2^attempt, 30 s)`:

| attempt | 0 | 1 | 2 | 3 | 4 | 5+ |
|---|---|---|---|---|---|---|
| delay | 1 s | 2 s | 4 s | 8 s | 16 s | 30 s |

Doubling stops at the cap, so huge attempt counts can't overflow. The worker
counts *consecutive failed passes*; one successful pass resets it. The policy is
deterministic (no jitter yet), so every delay is tested exactly.

## Failure Simulation

The server injects failures, chosen with `--mode` or from its menu while it runs:

| # | Mode | Stored? | Answered? | What the POS must do |
|---|---|---|---|---|
| 1 | `normal` | yes | yes | — |
| 2 | `latency` | yes | after 500 ms | wait |
| 3 | `random` | maybe | maybe | per request: 50 % normal, 25 % drop-before, 25 % drop-after |
| 4 | `timeout` | no | never | give up after its timeout, retry |
| 5 | `drop-before` | no | no | **retry is required** |
| 6 | `drop-after` | **yes** | no | **retry must be idempotent** |
| 7 | `reject` | no | 503 | retry later |

**Drop before processing** and **drop after processing** look identical to the
till — both are a `NetworkError` — but they are opposite situations:

```
DROP BEFORE PROCESSING                 DROP AFTER PROCESSING

POS ───X──→ server                     POS ─────→ server
                                                    ↓
server doesn't have it.                           COMMIT
Retry is required.                                  ↓
                                       POS ←──X── response

                                       server already has it.
                                       Retry must be idempotent.
```

Since the till cannot tell them apart, it always retries the same ID. Tests
cover both (`drop before processing needs the retry to deliver`,
`LostResponseDoesNotCauseDuplicateTransaction`), and one test fires 30 sales
through `random` mode and asserts the server ends with exactly 30 — none lost,
none duplicated.

## Threading Model

```
UI thread ──────── TransactionService ──┐
                                        ├── SQLiteTransactionRepository (std::mutex) ── SQLite
Sync worker (std::jthread) ─ SyncEngine ┘
                                        └── MonitoredServerClient (std::mutex) ── HttpServerClient
Server: one io_context thread (no locks needed in the store)
```

- **`std::jthread` + RAII shutdown.** `SyncEngine::start()` launches the worker.
  `stop()` — or simply the destructor — calls `request_stop()`, which wakes the
  worker out of any wait (the wait is registered with the `stop_token`); the
  in-flight send finishes; the thread joins. In `main()` the engine is declared
  after the repository and client, so destruction order *is* shutdown order:
  worker joins, then the client goes, then SQLite closes.
- **The worker sleeps when there is nothing to do rather than polling the database.**
  It waits on a `std::condition_variable_any` (C++20 `stop_token` overload).
  With an empty queue it waits indefinitely for `notifyPendingTransaction()`;
  after a failure it does a timed wait for the backoff delay. A test asserts an
  idle worker issues zero queries.
- **No retry storms.** New sales during a backoff are remembered but don't wake
  the worker; only `notifyServerReachable()` (a live sale just went through, so
  the outage is over) cuts a backoff short.
- **Serialized repository access.** One SQLite connection shared by both threads;
  every repository call holds one `std::mutex` (`std::scoped_lock`) for its whole
  duration, so validated status changes are atomic read-check-writes. Simple,
  fast enough for a till, and easy to reason about — no lock-free cleverness.
- **Sends happen outside every lock.** A send can take seconds; holding the
  repository lock during it would freeze the till.
- **Verified under ThreadSanitizer** in CI, together with ASan/UBSan.

## Technology

| | |
|---|---|
| Language | C++20 (`std::jthread`, `std::stop_token`, `condition_variable_any`, designated initializers, `std::format`, ranges) |
| Build | CMake ≥ 3.25, dependencies via `FetchContent` (pinned; SQLite and Boost hash-checked) |
| Storage | SQLite 3.46.1 amalgamation, WAL + `synchronous=FULL`, RAII handles/statements |
| Network | Boost 1.86 — Asio + Beast (HTTP/1.1), Boost.JSON |
| Logging | spdlog 1.14 |
| Tests | Catch2 v3 |
| CI | GitHub Actions: Ubuntu + Windows builds, ASan/UBSan, TSan, clang-tidy |

## Building

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Requirements: CMake ≥ 3.25 and a C++20 compiler (GCC 13+, Clang 16+, MSVC 2022+).
No system packages are needed; the first configure downloads the dependencies.

With sanitizers (GCC/Clang):

```bash
cmake -S . -B build-tsan -DRESILIENTPOS_SANITIZE=thread
cmake -S . -B build-asan -DRESILIENTPOS_SANITIZE=address,undefined
```

## Running

```bash
# terminal 1 — the central server (pick failure modes from its menu: 1-7)
./build/resilient-server

# terminal 2 — the till
./build/resilient-pos

# terminal 3 — the till's diagnostic log
tail -f data/pos.log
```

Cashier output and diagnostics are separate concerns: the CLI prints to the
terminal, while the engine logs structured lines through spdlog to
`data/pos.log` (the server logs to its console):

```
[2026-10-31 11:02:17] [INFO] Transaction created id=dfd39240 amount=700
[2026-10-31 11:02:19] [WARN] Server unavailable id=dfd39240 result=NETWORK_ERROR reason="Cannot connect to 127.0.0.1:8080: ..."
[2026-10-31 11:02:19] [INFO] Transaction queued id=dfd39240 status=PENDING_SYNC
[2026-10-31 11:02:21] [WARN] Sync retry scheduled attempt=1 delay_ms=1000
```

## Testing

```bash
ctest --test-dir build --output-on-failure
```

103 tests. Logging is silenced in tests; set `RESILIENTPOS_TEST_LOGS=1` to see it.

| Suite | Covers |
|---|---|
| `MoneyTests` | constructs from cents, adds, multiplies, formats, parses cashier input, overflow |
| `TransactionTests`, `UuidGeneratorTests` | unique v4 IDs, starts `CREATED`, timestamp |
| `TransactionStatusTests` | string round-trips; the full 4×4 transition table; nonsense rejected |
| `RepositoryTests` | save/load, status updates, pending queue order, compare-and-set claims, sync-attempt counting, crash recovery, survives reopen |
| `RetryPolicyTests` | 1 s / 2 s / 4 s ..., cap, no overflow |
| `TransactionServiceTests` | persists before sending, success → `SYNCED`, failure → `PENDING_SYNC`, storage failure sends nothing |
| `SyncEngineTests` | loads and sends pending oldest first, success → `SYNCED`, network failure stays pending and ends the pass, `ALREADY_PROCESSED` → `SYNCED`, crash recovery |
| `SyncWorkerTests` | worker wakes on notify, idle worker doesn't poll, exponential backoff timing, no retry storm, prompt shutdown, concurrent claims, selling while syncing loses/duplicates nothing |
| `ServerTests` | new ID accepted, duplicate not processed twice, duplicate → `ALREADY_PROCESSED`, malformed requests rejected |
| `IntegrationTests` | real sockets + real SQLite: online, offline recovery (manual and background), crash recovery, **`LostResponseDoesNotCauseDuplicateTransaction`**, drop-before, random failures, every simulation mode |

Unit tests use test-only fakes (`tests/fakes/FakeTransactionRepository.hpp`,
`tests/fakes/FakeServerClient.hpp`), so the sync engine is tested without SQLite,
sockets or an HTTP server. Integration tests start a real server on a free port.

## Design Trade-offs

- **One mutex around one SQLite connection** instead of a connection pool or
  lock-free queue. A till does a few writes a minute; correctness and
  simplicity win. A server-side store would need more.
- **Global backoff, not per-transaction.** When the server is down it is down for
  every sale, so one schedule for the whole queue is both simpler and kinder to
  the server. A poison message (one sale the server always rejects with an
  error) would keep the pass carrying on past it; a dead-letter state would be
  the next step.
- **No jitter yet.** Deterministic delays keep tests exact. A fleet of tills
  should add jitter so they don't all retry in lockstep after an outage.
- **Connectivity is learned from real traffic** (`MonitoredServerClient`) rather
  than a separate health-check loop: no extra requests, but the status is only as
  fresh as the last send.
- **Synchronous HTTP client per call.** Each send uses a private `io_context`
  with async operations purely to get timeouts. Simple and thread-safe; a
  high-throughput client would keep a connection open.
- **Single-threaded server.** One `io_context` thread means store writes never
  race. Idempotency is still enforced by the database (primary key + `ON CONFLICT`),
  so a multi-threaded or multi-instance server would stay correct.
- **Amounts only.** The wire format carries the ID and the total in integer cents;
  line items, payment methods and receipts are out of scope.

### Schemas

```sql
-- data/pos.db
CREATE TABLE IF NOT EXISTS transactions (
    id TEXT PRIMARY KEY,
    amount_cents INTEGER NOT NULL,
    status TEXT NOT NULL,           -- CREATED, PENDING_SYNC, SYNCING, SYNCED
    created_at INTEGER NOT NULL,    -- milliseconds since Unix epoch
    sync_attempts INTEGER NOT NULL DEFAULT 0
);

-- data/server.db
CREATE TABLE IF NOT EXISTS processed_transactions (
    id TEXT PRIMARY KEY,
    amount_cents INTEGER NOT NULL,
    processed_at INTEGER NOT NULL   -- milliseconds since Unix epoch
);
```

### Layout

```
include/resilientpos/   public headers: core, storage, network, service, sync, server, util
src/                    implementation, mirroring include/ (+ internal SQLite helpers)
apps/pos/               resilient-pos — the till
apps/server/            resilient-server — the central server
tests/                  unit + integration tests, tests/fakes/ test doubles
docs/                   demo script
data/                   runtime databases and logs (ignored by git)
```
