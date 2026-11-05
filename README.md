# ResilientPOS

**An offline-first Point-of-Sale engine in modern C++20 that keeps selling through network failures.**

![CI](https://github.com/kgiannopoulou/ResilientPOS/actions/workflows/ci.yml/badge.svg)

A till persists every sale locally *before* it touches the network, queues
whatever the server did not confirm, and a background worker drains the queue
with jittered exponential backoff once connectivity returns. The server is
idempotent by transaction ID and payload, so a retry after a lost response can
never charge a customer twice, and a reused ID with different contents is
refused rather than silently accepted. Sales that can never succeed move to a
dead letter queue for a person to inspect. A simulation runs a whole fleet of
terminals through an outage and proves nothing was lost or doubled.

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
   short outage into a long one, and a hundred tills retrying *in step* can knock
   a recovering server straight back over.
4. **Giving up at the wrong time.** A sale the server will never accept must not
   be retried forever, but a till that has been offline for two days must not
   give up on anything.

ResilientPOS is built around solving exactly those four, and proving it with tests.

## Demo

Three programs, each with its own SQLite databases:

```
./build/resilient-server [--mode normal|latency|random|timeout|drop-before|drop-after|reject|poison] [--threads 4] [port=8080] [database=data/server.db]
./build/resilient-pos    [--metrics-port N] [database=data/pos.db] [server-host=127.0.0.1] [server-port=8080]
./build/resilient-sim    [--terminals 3] [--sales 5] [--threads 4] [--dir data/simulation]
```

**Online sale** — server running normally:

```
========================================
              ResilientPOS
========================================

Network: ONLINE
Pending transactions: 0
Dead letters: 0

1. New transaction
2. List transactions
3. Force synchronization
4. Network status
5. Dead letter queue
6. Exit

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

**A whole shop through an outage** — `resilient-sim` runs real terminals against
a real server, then checks every sale ([details](#multi-terminal-simulation)):

```
=== 3. The central server goes down ===
Every terminal keeps selling and backs off on its own schedule...

TERMINAL  LINK     SALES SYNCED  PENDING     DEAD   RETRIES
POS-1     DOWN        15      5       10        0        19
POS-2     up          15     10        5        0         4
POS-3     up          15     10        5        0         4
SERVER    DOWN   stored 25

=== 7. Verification ===
Sales rung up on terminals:   45
Unique transaction IDs:       45
Rows on the server:           45
Missing on the server:        0
Wrong amount on the server:   0
Till total / server total:    €270.00 / €270.00
Duplicates absorbed:          3
Conflicts refused:            1

PASS: every sale is on the server exactly once.
```

A scripted walkthrough is in [docs/DEMO.md](docs/DEMO.md).

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
                                     (N I/O threads, a strand per connection)
                                           │
                                           ▼
                                  handleTransactionRequest()
                                           │
                                           ▼
                                  ProcessedTransactionStore ── data/server.db
                                     (connection pool)


                   ┌──────────────────┐
                   │    SyncEngine    │   std::jthread worker + RetryPolicy + dead letters
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
   `ALREADY_PROCESSED` and never overwrites the stored amount. Enforced by the
   database, so it holds across threads and across server processes.
   *Test: `The database alone keeps separate writers from double-inserting`.*
5. **A lost response does not cause duplicate processing.** The till retries
   with the *same* ID; `ALREADY_PROCESSED` counts as success and the sale ends
   `SYNCED`. *Test: `IntegrationTest: LostResponseDoesNotCauseDuplicateTransaction`.*
6. **A reused ID with different contents is never accepted.** Same ID, different
   payload answers `409 IDEMPOTENCY_CONFLICT`, writes nothing and leaves the
   original intact. *Tests: `Server answers 409 when an ID comes back with a
   different payload`, `Concurrent conflicting requests: one wins, the rest get 409`.*
7. **Synchronization retries use exponential backoff with full jitter.** Each wait
   is random within a 1 s, 2 s, 4 s, 8 s, 16 s, then 30 s window, so a fleet does
   not retry in step. New sales during a backoff do not cut it short.
   *Tests: `Worker retries a failing server with exponential backoff`,
   `New sales during a backoff do not trigger a retry storm`,
   `Jitter spreads a fleet that failed at the same moment`.*
8. **Being offline never costs a sale.** Only failures the server attributes to a
   request use up its retry budget. *Test: `A long outage never dead-letters a sale`.*
9. **A sale is never deleted.** One that cannot succeed moves to `DEAD_LETTER`, stays
   on disk with the reason, and a person can requeue it.
   *Tests: `Failure details and dead letters survive a restart`,
   `Manual retry requeues a dead letter and it can then sync`.*

## Persist First, Send Second

```
create transaction
       │
       ▼
save to SQLite as CREATED    ◄── the sale is now safe; a crash here loses nothing
       │
       ▼
send to server
     /      |       \
 success  failure   server refused the sale itself
    │        │         │
 SYNCED  PENDING_SYNC  DEAD_LETTER   the till carries on either way
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

200 {"status": "ACCEPTED"}                       first time this ID is seen
200 {"status": "ALREADY_PROCESSED"}              same ID, same payload: nothing written
409 {"status": "IDEMPOTENCY_CONFLICT", ...}      same ID, DIFFERENT payload: refused
400 {"status": "REJECTED", "error": "..."}       malformed request
422 {"status": "REJECTED", "error": "..."}       invalid business data
500 {"status": "ERROR", "error": "..."}          storage failure
503 {"status": "ERROR", "error": "..."}          database busy, or --mode reject
```

Both 200 answers mean "the server has it". `SendResult::successful()` is true
for both, so the till marks the sale `SYNCED` either way.

An idempotency key is only safe if it stands for one specific request. Without
that check, this is what a retry looks like:

```
POST {"transaction_id": "abc-123", "amount_cents": 1000}   200 ACCEPTED
POST {"transaction_id": "abc-123", "amount_cents": 10000}  200 ALREADY_PROCESSED  <- wrong
```

The till believes a €100 sale is safely stored, the server holds €10, and nothing
ever reports the difference. So each row also stores a **payload hash**: SHA-256
of a versioned canonical form of everything except the ID.

```
same ID + same hash       ALREADY_PROCESSED
same ID + different hash  409 IDEMPOTENCY_CONFLICT, logged, counted, nothing written
```

The server computes the hash from what it received; it never trusts one from the
client. Comparing a hash rather than `amount_cents` means line items only have to
be added to the canonical form (with a new version prefix, so stored hashes stay
valid), and the check itself does not change. Older databases are upgraded and
backfilled when opened.

The order of operations is what keeps this race-free: the atomic insert goes
first, and only the loser reads the stored hash. Rows are never updated or
deleted, so what the loser reads is final no matter who won.

## Transaction State Machine

```
CREATED ──────────────────────────────► SYNCED      first send accepted
   │  │                                   ▲
   │  └─────────────────────┐             │
   ▼                        │             │
PENDING_SYNC ──► SYNCING ───┼─────────────┘         resend accepted
   ▲  ▲             │  │    │
   │  └─────────────┘  │    │                       resend failed, will retry
   │                   ▼    ▼
   └──────────── DEAD_LETTER                        gave up; a person may requeue
```

| Status | Meaning | Owned by |
|---|---|---|
| `CREATED` | on disk, first send in flight | `TransactionService` |
| `PENDING_SYNC` | waiting in the offline queue | nobody (claimable) |
| `SYNCING` | a resend is in flight | `SyncEngine` |
| `SYNCED` | the server holds it — final | — |
| `DEAD_LETTER` | given up on, kept for inspection | an operator |

`canTransition(from, to)` allows exactly these eight moves; anything else
(`SYNCED → CREATED`, `PENDING_SYNC → SYNCED`, `DEAD_LETTER → SYNCED`, ...) throws
`InvalidTransitionError`, enforced inside the repository. Two details matter:

- **Ownership by state.** The worker only takes `PENDING_SYNC` rows, via an atomic
  compare-and-set (`tryUpdateStatus(id, PENDING_SYNC, SYNCING)`). A live sale is
  `CREATED` while the service sends it, so the two threads can never send the
  same sale at once.
- **Crash recovery.** A `CREATED` or `SYNCING` row on disk at startup belongs to a
  run that died mid-send. `recoverInterrupted()` moves it back to `PENDING_SYNC`
  (both legal moves) and it is resent; idempotency makes that safe whether or not
  the server got the first copy. Dead letters are not in flight and stay put.

## Dead Letter Queue

Retrying only helps if the next attempt can go differently. So every failure is
classified by what it says about the sale:

| Class | Results | Retried? | Uses retry budget? |
|---|---|---|---|
| Unreachable | network down, timeout, 408/429/502/503/504 | forever | **no** |
| Transient | 500 and other unexplained errors | up to the limit (10) | yes, then `DEAD_LETTER` |
| Permanent | 400, 413, 422, 409 | never | straight to `DEAD_LETTER` |

The first row is the design decision that matters. A naive "max attempts" would
dead-letter the sale at the head of the queue after a few minutes offline, which
defeats the point of an offline-first till. Unreachable failures say nothing about
the sale, so they never count, and a till can stay cut off for days. A 503 counts
as unreachable for the same reason: the server is shedding load, not judging this
sale. A 404 is deliberately *not* permanent, because a misconfigured proxy must
not bin every sale on the first try.

Each row records `failed_attempts`, `last_failure_code`, `last_failure_reason` and
`last_failure_at`, and all of it survives restarts. Dead letters don't block the
queue behind them, and they trigger no backoff: the server is fine, that sale isn't.

From the till's menu, or through `SyncEngine::retryDeadLetter()`:

```
1 transaction(s) need attention. They are kept locally and are not being retried.

#   ID           AMOUNT       FAILURE               RESENDS  LAST FAILURE
--------------------------------------------------------------------------------------
1   8e440116...  €7.00        REJECTED              0        2026-11-05 08:52:31 UTC
    Reason: HTTP 422: {"status":"REJECTED","error":"simulated validation failure"}

Number to retry, 'a' to retry all, Enter to go back:
```

A manual retry moves the sale back to `PENDING_SYNC` with a fresh retry budget and
wakes the worker. The failure details stay as a record of what happened.

## Offline Synchronization

`SyncEngine::syncPendingTransactions()` is one pass: load pending rows oldest
first, claim each one, send it, then mark it `SYNCED`, return it to `PENDING_SYNC`
or move it to `DEAD_LETTER`.

- An **unreachable server ends the pass early.** If it is unreachable for one sale
  it is unreachable for all of them, and each attempt can take seconds.
- **Any other failure is about that one request**, so the pass carries on: one
  poison sale cannot hold up the queue behind it.
- `ALREADY_PROCESSED` is success, counted separately in the `SyncReport`.

The CLI's *Force synchronization* runs a pass on the UI thread; passes are
serialized, so it can never overlap the background worker's.

## Retry Strategy

Exponential backoff with **full jitter**:

```
ceiling = min(1 s × 2^attempt, 30 s)
delay   = random(0, ceiling)
```

| attempt | 0 | 1 | 2 | 3 | 4 | 5+ |
|---|---|---|---|---|---|---|
| ceiling | 1 s | 2 s | 4 s | 8 s | 16 s | 30 s |
| a possible delay | 0.6 s | 1.3 s | 2.1 s | 5.7 s | 9.2 s | 21 s |

Backoff alone spaces out one till's retries, not a fleet's. A hundred tills that
lost the server at the same moment share the same schedule and would all arrive
together at 1 s, 3 s, 7 s ...: a thundering herd, aimed at a server that has only
just recovered. Spreading each wait over the whole window breaks the lockstep.

The source of randomness is an injected `std::function`, so nothing about the
tests is random: `noJitter()` gives exact timing, `fullJitter(seed)` gives
reproducible sequences, and a lambda can pick any delay a test needs. The default
seeds from `std::random_device`, so tills started in the same instant still differ.

Doubling stops at the cap, so huge attempt counts can't overflow. The worker
counts *consecutive failed passes*; one successful pass resets it.

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
| 8 | `poison` | no | 422 | dead-letter it, never retry |

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

## Multi-Terminal Simulation

`resilient-sim` runs a whole shop for real, with no fakes: N terminals, each with
its own SQLite file, sync worker and metrics, talking HTTP over TCP to one
multi-threaded server.

```
POS-1 ──┐                                      own IDs, own queue,
POS-2 ──┼──► central server ──► server.db      own retry schedule
POS-3 ──┘
```

| # | Phase | What it proves |
|---|---|---|
| 1 | Normal trading | the baseline |
| 2 | POS-1 alone loses its link | **fault isolation**: the others are unaffected |
| 3 | The server stops | every terminal keeps selling and backs off independently |
| 4 | Server returns, drops replies after committing | the ambiguous failure, at fleet scale |
| 5 | Network healthy | queues drain with no duplicates |
| 6 | Tampered replay of a known ID | refused, parked in the dead letter queue, original unchanged |
| 7 | Verification | every sale compared with the server, by ID and by amount |

It exits non-zero unless every sale is on the server exactly once, so CI runs it
on every push. The same scenario is a test (`Fleet: every sale reaches the server
exactly once, through every failure`), which also puts it under ThreadSanitizer.
The delays each terminal drew are printed at the end, which makes the jitter visible:

```
POS-1: 45ms 19ms 23ms 155ms 61ms 262ms 696ms 592ms ... (20 in all)
POS-2: 36ms 172ms 88ms 677ms 726ms 541ms
POS-3: 57ms 36ms 324ms 764ms 243ms 534ms
```

## Observability

Logs answer "what happened to this sale?". Metrics answer "is the fleet healthy?",
so both programs serve the same three endpoints:

| Endpoint | Question | Answer |
|---|---|---|
| `GET /health` | is the process alive? | `200 {"status":"UP"}` |
| `GET /ready` | can it do its job? | `200 READY` or `503 NOT_READY`, with its checks |
| `GET /metrics` | how is it doing? | Prometheus text format |

The server serves them on its main port; the till opens a listener with
`--metrics-port`. They answer differently on purpose: a failing `/health` means
restart it, a failing `/ready` means send it no traffic yet. A till that is offline
is still *ready*, since all it needs to take a sale is its database. The endpoints
ignore simulation modes, because monitoring has to be able to see an outage.

Till metrics:

| Metric | Type | What it tells you |
|---|---|---|
| `transactions_created_total` | counter | sales rung up |
| `transactions_synced_total` | counter | sales the server confirmed |
| `transactions_pending` | gauge | current offline queue size |
| `transactions_dead_letter` | gauge | current dead letter queue size |
| `transactions_dead_lettered_total` | counter | sales given up on |
| `sync_failures_total` | counter | failed send attempts |
| `sync_duration_ms` | histogram | send latency |
| `retry_attempts_total` | counter | resends by the sync engine |
| `idempotency_conflicts_total` | counter | sales refused for a conflicting ID |

Server metrics: `server_requests_total`, `server_transactions_accepted_total`,
`server_transactions_duplicate_total`, `server_transactions_rejected_total`,
`server_errors_total`, `idempotency_conflicts_total`,
`server_request_duration_ms` (histogram), and the gauges
`server_transactions_stored` and `server_io_threads`.

```
$ curl localhost:8080/metrics
# HELP idempotency_conflicts_total Requests reusing an ID with a different payload
# TYPE idempotency_conflicts_total counter
idempotency_conflicts_total 1
...
$ curl localhost:8080/ready
{"status":"READY","checks":{"database":"ok","simulation_mode":"normal"}}
```

Counters are lock-free atomics, so instrumenting a request costs no lock. Queue
sizes are gauges read from the database at scrape time rather than counters kept
in step with it, so they cannot drift. To scrape with Prometheus:

```yaml
scrape_configs:
  - job_name: resilientpos
    static_configs:
      - targets: ["localhost:8080"]   # server; add each till's --metrics-port
```

## Threading Model

```
UI thread ──────── TransactionService ──┐
                                        ├── SQLiteTransactionRepository (std::mutex) ── SQLite
Sync worker (std::jthread) ─ SyncEngine ┘
                                        └── MonitoredServerClient (std::mutex) ── HttpServerClient

                 POS 1    POS 2    POS 3
                    \       |       /
                  acceptor (its own strand)
                            |
          one strand per connection, on a shared io_context
                  /         |         \
              I/O 1       I/O 2      I/O 3       --threads N
                  \         |         /
              ProcessedTransactionStore          connection pool
                            |
              SQLite: PRIMARY KEY + atomic INSERT
```

On the till:

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

On the server:

- **A strand per connection.** One connection's handlers never run in parallel, so
  a `Session` needs no locks, while different connections run on different threads.
  The acceptor has its own strand, which is also where `stop()` closes it.
- **Why threads help.** A store call blocks its I/O thread for a commit (an fsync
  with `synchronous=FULL`). Other threads keep reading and answering meanwhile. A
  test proves the parallelism directly: requests wait inside the server until
  several are in there at once, which can only happen on separate threads.
- **A connection pool, not a shared connection.** Each store call borrows a
  connection for its whole duration. `sqlite3_changes()`, which decides whether an
  insert won, is per connection, so sharing one would be a real race.
- **Shared state is kept to two things:** the store, and the simulation mode (an
  atomic). The `RandomFailure` generator was safe with one thread and became a data
  race with several, so it is now `thread_local`.
- **The database, not a lock, guarantees idempotency.** Writers are deliberately
  not serialized in the process. An in-process mutex would pass every test here and
  still break the moment a second server shared the file, so the guarantee rests on
  the primary key and one atomic statement. `The database alone keeps separate
  writers from double-inserting` proves it by racing independent store instances
  that share nothing but the file.
- **Contention degrades gracefully.** A write that outlasts SQLite's busy timeout
  answers 503, which the till treats as "try again later" without using any of that
  sale's retry budget.

Both sides are verified under ThreadSanitizer in CI, together with ASan/UBSan.

## Technology

| | |
|---|---|
| Language | C++20 (`std::jthread`, `std::stop_token`, `condition_variable_any`, `std::latch`, designated initializers, `std::format`, ranges) |
| Build | CMake ≥ 3.25, dependencies via `FetchContent` (pinned; SQLite and Boost hash-checked) |
| Storage | SQLite 3.46.1 amalgamation, WAL + `synchronous=FULL`, RAII handles/statements |
| Network | Boost 1.86 — Asio + Beast (HTTP/1.1), Boost.JSON |
| Logging | spdlog 1.15 |
| Metrics | own lock-free registry, Prometheus text format |
| Tests | Catch2 v3 |
| CI | GitHub Actions: Ubuntu + Windows builds, multi-terminal simulation, ASan/UBSan, TSan, clang-tidy |

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
# terminal 1 — the central server (pick failure modes from its menu: 1-8)
./build/resilient-server --threads 4

# terminal 2 — the till, with its metrics on port 9100
./build/resilient-pos --metrics-port 9100

# terminal 3 — the till's diagnostic log
tail -f data/pos.log

# or a whole shop in one command
./build/resilient-sim --terminals 5
```

Cashier output and diagnostics are separate concerns: the CLI prints to the
terminal, while the engine logs structured lines through spdlog to
`data/pos.log` (the server logs to its console):

```
[2026-10-31 11:02:17] [INFO] Transaction created id=dfd39240 amount=700
[2026-10-31 11:02:19] [WARN] Server unavailable id=dfd39240 result=NETWORK_ERROR reason="Cannot connect to 127.0.0.1:8080: ..."
[2026-10-31 11:02:19] [INFO] Transaction queued id=dfd39240 status=PENDING_SYNC
[2026-10-31 11:02:21] [WARN] Sync retry scheduled attempt=1 delay_ms=613 ceiling_ms=1000
```

## Testing

```bash
ctest --test-dir build --output-on-failure
```

187 tests. Logging is silenced in tests; set `RESILIENTPOS_TEST_LOGS=1` to see it.

| Suite | Covers |
|---|---|
| `MoneyTests` | constructs from cents, adds, multiplies, formats, parses cashier input, overflow |
| `TransactionTests`, `UuidGeneratorTests` | unique v4 IDs, starts `CREATED`, timestamp |
| `Sha256Tests` | NIST vectors, padding around the block boundary |
| `TransactionStatusTests` | string round-trips; the full 5×5 transition table; nonsense rejected |
| `RepositoryTests` | save/load, status updates, pending queue order, compare-and-set claims, sync-attempt counting, crash recovery, survives reopen |
| `RetryPolicyTests` | ceilings, cap, no overflow; jitter stays in the window, covers it, is reproducible from a seed, and spreads a fleet of 100 |
| `TransactionServiceTests` | persists before sending, success → `SYNCED`, failure → `PENDING_SYNC`, storage failure sends nothing |
| `SyncEngineTests` | loads and sends pending oldest first, success → `SYNCED`, network failure stays pending and ends the pass, `ALREADY_PROCESSED` → `SYNCED`, crash recovery |
| `SyncWorkerTests` | worker wakes on notify, idle worker doesn't poll, exponential backoff timing, no retry storm, prompt shutdown, concurrent claims, selling while syncing loses/duplicates nothing |
| `ServerTests` | new ID accepted, duplicate not processed twice, duplicate → `ALREADY_PROCESSED`, conflicting payload → 409, schema upgrade, malformed requests rejected |
| `DeadLetterTests` | failure classification, retry limit, a long outage never dead-letters, poison sale doesn't block the queue, no backoff, manual retry, restart, schema upgrade |
| `ConcurrencyTests` | racing inserts and conflicts, independent store instances on one file, pool exhaustion, busy → 503, proven parallelism, concurrent duplicates and conflicts over HTTP, stress with many clients, shutdown under load |
| `MetricsTests` | Prometheus rendering, histogram buckets, every till and server metric, `/health` `/ready` `/metrics` over HTTP, monitoring during a simulated outage |
| `FleetSimulationTests` | the whole multi-terminal scenario, a larger fleet, and that a report which doesn't add up fails |
| `IntegrationTests` | real sockets + real SQLite: online, offline recovery (manual and background), crash recovery, **`LostResponseDoesNotCauseDuplicateTransaction`**, drop-before, random failures, every simulation mode |

Unit tests use test-only fakes (`tests/fakes/FakeTransactionRepository.hpp`,
`tests/fakes/FakeServerClient.hpp`), so the sync engine is tested without SQLite,
sockets or an HTTP server. Integration tests start a real server on a free port.

## Design Trade-offs

- **One mutex around one SQLite connection on the till**, instead of a connection
  pool or lock-free queue. A till does a few writes a minute; correctness and
  simplicity win. The server's store, which does need more, has a pool.
- **SQLite on the server.** WAL allows one writer at a time, so extra I/O threads
  buy overlap rather than parallel commits. Enough for one shop's tills; a chain
  would swap in a client/server database, and because idempotency is a primary key
  plus one atomic statement, that design carries over unchanged.
- **Global backoff, not per-transaction.** When the server is down it is down for
  every sale, so one schedule for the whole queue is both simpler and kinder to
  the server. A sale that keeps failing on its own is handled by the retry limit
  and the dead letter queue instead.
- **Full jitter over equal or decorrelated jitter.** It can pick a very short wait,
  but it spreads load the most for the least code. Ceilings stay exactly testable.
- **A server-wide fault can dead-letter a backlog.** Ten straight 500s on a queue
  will park those sales. Nothing is lost, and "retry all" brings them back, but the
  limit counts attempts rather than elapsed time. Storage contention avoids this by
  answering 503.
- **Conflicts are only reported, not resolved.** The server keeps the first payload
  and the till parks the other. Deciding which sale was right needs a person.
- **Metrics reset on restart.** Prometheus handles counter resets natively, and the
  numbers that must survive live in the database.
- **Connectivity is learned from real traffic** (`MonitoredServerClient`) rather
  than a separate health-check loop: no extra requests, but the status is only as
  fresh as the last send.
- **Synchronous HTTP client per call.** Each send uses a private `io_context`
  with async operations purely to get timeouts. Simple and thread-safe; a
  high-throughput client would keep a connection open.
- **Blocking store calls on I/O threads.** Simple, and the thread count bounds
  concurrent commits. A separate worker pool would keep I/O threads free at the cost
  of another hand-off; at this load it would buy nothing.
- **Amounts only.** The wire format carries the ID and the total in integer cents;
  line items, payment methods and receipts are out of scope. The payload hash is
  built so that adding them doesn't change the idempotency check.

### Schemas

```sql
-- data/pos.db
CREATE TABLE IF NOT EXISTS transactions (
    id TEXT PRIMARY KEY,
    amount_cents INTEGER NOT NULL,
    status TEXT NOT NULL,           -- CREATED, PENDING_SYNC, SYNCING, SYNCED, DEAD_LETTER
    created_at INTEGER NOT NULL,    -- milliseconds since Unix epoch
    sync_attempts INTEGER NOT NULL DEFAULT 0,    -- every resend
    failed_attempts INTEGER NOT NULL DEFAULT 0,  -- those that count against the limit
    last_failure_code TEXT,         -- e.g. REJECTED, NETWORK_ERROR
    last_failure_reason TEXT,
    last_failure_at INTEGER
);

-- data/server.db
CREATE TABLE IF NOT EXISTS processed_transactions (
    id TEXT PRIMARY KEY,
    amount_cents INTEGER NOT NULL,
    processed_at INTEGER NOT NULL,  -- milliseconds since Unix epoch
    payload_hash TEXT NOT NULL      -- SHA-256 of the canonical payload
);
```

### Layout

```
include/resilientpos/   public headers: core, storage, network, service, sync, server,
                        observability, simulation, util
src/                    implementation, mirroring include/ (+ internal SQLite helpers)
apps/pos/               resilient-pos — the till
apps/server/            resilient-server — the central server
apps/simulation/        resilient-sim — a whole shop through an outage
tests/                  unit + integration tests, tests/fakes/ test doubles
docs/                   demo script
data/                   runtime databases and logs (ignored by git)
```
