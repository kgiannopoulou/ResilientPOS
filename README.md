# ResilientPOS

An **offline-first Point-of-Sale engine** written in modern C++20.
The till keeps taking sales when the network drops, persists everything locally,
and synchronises with the server once connectivity returns.

> Status: POS and central server talk over HTTP; the till keeps selling when the server is down.
> - Milestone #1: **a POS crash/restart doesn't destroy pending transactions.**
> - Milestone #2: **online transactions reach the server and are marked `SYNCED`.**
> - Milestone #3: **with the server off, sales are saved locally as `PENDING_SYNC` and the POS keeps operating.**

![CI](https://github.com/kgiannopoulou/ResilientPOS/actions/workflows/ci.yml/badge.svg)

## Build

```bash
cmake -S . -B build
cmake --build build
```

This produces two programs, each with **its own database**:

| Program | Database | Arguments |
|---|---|---|
| `./build/resilient-server` | `data/server.db` | `[port=8080] [database]` |
| `./build/resilient-pos` | `data/pos.db` | `[database] [server-host=127.0.0.1] [server-port=8080]` |

### Online sale (milestone #2)

Terminal 1:

```
$ ./build/resilient-server
======= ResilientPOS Server ========
Database: data/server.db (0 processed)
Listening on http://127.0.0.1:8080/transactions
Press Ctrl+C to stop.

[SERVER] {"transaction_id":"37a64d74-2204-4c60-af91-200ef66c19ad","amount_cents":700} -> {"status":"ACCEPTED"}
```

Terminal 2:

```
$ ./build/resilient-pos
Choice: 1

Product: Coffee
Price cents: 350
Quantity: 2

2 x Coffee @ €3.50
Total: €7.00

Processing transaction...

[NETWORK] Connected
[SERVER] Transaction accepted

Transaction: 37a64d74-2204-4c60-af91-200ef66c19ad
Status: SYNCED
```

Both databases agree:

```
data/pos.db     transactions            37a64d74-...  700  SYNCED
data/server.db  processed_transactions  37a64d74-...  700
```

Sending the same transaction again is harmless:

```
$ curl -X POST http://127.0.0.1:8080/transactions \
       -d '{"transaction_id":"37a64d74-2204-4c60-af91-200ef66c19ad","amount_cents":700}'
{"status":"ALREADY_PROCESSED"}
```

### Offline sales (milestone #3)

Stop the server (`Ctrl+C`) and keep selling:

```
Processing transaction...

[NETWORK] Server unavailable (Cannot connect to 127.0.0.1:8080: ... actively refused it)

Transaction saved locally.

Transaction: 4a9b7017-19ab-4ccc-be8e-c8bc040ddd31
Status: PENDING_SYNC

POS CAN CONTINUE OPERATING
```

`2. List pending`, and again after restarting the POS:

```
ID          AMOUNT       STATUS         CREATED
--------------------------------------------------------------
4a9b7017    €5.50        PENDING_SYNC   2026-10-29 11:00:32 UTC
5137b118    €12.40       PENDING_SYNC   2026-10-29 11:00:34 UTC
42537391    €3.20        PENDING_SYNC   2026-10-29 11:00:36 UTC

Pending synchronization: 3
Pending total: €21.10
```

### Tests

```bash
ctest --test-dir build --output-on-failure
```

50 tests: Money, statuses, UUIDs, the SQLite repository, the server store's
idempotency, request validation, the persist-first ordering of
`TransactionService` (with a fake `ServerClient`), and real HTTP round trips
between `HttpServerClient` and `HttpTransactionServer` on a free local port.

Dependencies are fetched by CMake, pinned and hash-checked — no system packages needed:
[Catch2 v3](https://github.com/catchorg/Catch2),
the [SQLite amalgamation](https://www.sqlite.org/amalgamation.html) 3.46.1, and
[Boost](https://www.boost.org/) 1.86 (only Asio, Beast and JSON are configured).

Requirements: CMake ≥ 3.25 and a C++20 compiler (GCC 11+, Clang 14+, MSVC 2022+).

## Design notes

- **Persist first, send second.** `TransactionService::createTransaction()` saves
  the sale to SQLite *before* any network call; a test's fake `ServerClient`
  checks the row is already on disk when `send()` runs. The row is written as
  `PENDING_SYNC`, so if the till dies mid-request the sale is already queued for
  sync. After the reply it becomes `SYNCED` (accepted / already processed) or
  stays `PENDING_SYNC` (network or server error). Only a storage failure fails a sale.
- **Idempotent server.** The server's `processed_transactions` table uses the
  transaction UUID as `PRIMARY KEY` and records with
  `INSERT ... ON CONFLICT(id) DO NOTHING`. "Does it exist? If not, insert" is a
  single atomic statement: a resend answers `ALREADY_PROCESSED`, and the stored
  amount is never overwritten.
- **Duplicates are success.** `SendResult::successful()` is true for both
  `Accepted` and `AlreadyProcessed`: if the server stored a sale but the reply was
  lost, the retry comes back `AlreadyProcessed` and the till can still mark it `SYNCED`.
- **Failures are values, not exceptions.** `HttpServerClient` turns refused
  connections, DNS errors, timeouts (3 s per step, via async Beast operations on a
  private `io_context`) and dropped replies into `NetworkError`, and non-2xx or
  unexpected replies into `ServerError`. No retries yet — the sync layer will own that.
- **Money is integer cents.** `Money` wraps an `int64_t` count of cents, also on the
  wire (`"amount_cents": 700`); the server rejects non-integer amounts. Arithmetic is
  overflow-checked.
- **Transactions are born offline.** Every sale gets a random version-4 UUID on the
  till, so tills never need the server to create sales and IDs never collide.
- **Business logic depends on interfaces.** `TransactionService` only sees
  `TransactionRepository` and `ServerClient`. Boost is linked privately: no public
  header includes it, and the HTTP server hides Asio behind a pimpl.
- **Durable local storage.** Both databases use WAL with `synchronous=FULL`, so an
  acknowledged write is fsynced. SQLite handles are owned by RAII wrappers shared
  by the POS repository and the server store.
- **Single-threaded server.** `HttpTransactionServer` runs one `io_context` thread, so
  store writes never race; each session has a 10 s deadline and a 16 KiB body limit
  so a stalled client can't hold it.

```
   apps/pos (CLI)                                   apps/server
        │                                                │
        ▼                                                ▼
 TransactionService ──────► ServerClient        HttpTransactionServer (Beast)
        │                        ▲                       │
        ▼                        │                       ▼
 TransactionRepository    HttpServerClient ── HTTP ─► handleTransactionRequest()
        ▲                 (Asio/Beast, JSON)             │
        │                                                ▼
 SQLiteTransactionRepository                  ProcessedTransactionStore
        │                                                │
   data/pos.db                                     data/server.db
```

### Wire format

```
POST /transactions
{"transaction_id": "550e8400-e29b-41d4-a716-446655440000", "amount_cents": 700}

200 {"status": "ACCEPTED"}            first time this ID is seen
200 {"status": "ALREADY_PROCESSED"}   ID already stored, nothing written
400 {"status": "REJECTED", "error": "..."}   malformed request
500 {"status": "ERROR", "error": "..."}      storage failure
```

### Schemas

```sql
-- data/pos.db
CREATE TABLE IF NOT EXISTS transactions (
    id TEXT PRIMARY KEY,
    amount_cents INTEGER NOT NULL,
    status TEXT NOT NULL,           -- CREATED, PENDING_SYNC, SYNCING, SYNCED, FAILED
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

## Layout

```
include/resilientpos/   public headers (core, storage, network, service, server, sync, util)
src/                    implementation files, mirroring include/ (+ internal SQLite helpers)
apps/pos/               POS terminal executable
apps/server/            central server executable
tests/                  unit and HTTP integration tests
data/                   local runtime databases (ignored by git)
```

## Roadmap

- [x] Phase 1 — project skeleton (CMake, C++20, POS + server apps)
- [x] Phase 2 — `Money` value type (integer cents)
- [x] Phase 3 — `TransactionStatus` lifecycle
- [x] Phase 4 — `UuidGenerator` (UUID v4 transaction IDs)
- [x] Phase 5 — `Transaction` model
- [x] Phase 6 — `TransactionRepository` interface
- [x] Phase 7 — `SQLiteTransactionRepository` (RAII, durability tests)
- [x] Phase 8 — persistent POS CLI (**milestone #1**: survives restarts)
- [x] Phase 9 — `SendResult` (Accepted / AlreadyProcessed / NetworkError / ServerError)
- [x] Phase 10 — `ServerClient` interface
- [x] Phase 11 — mock central server with its own idempotent `processed_transactions` store
- [x] Phase 12 — `HttpServerClient` (Boost.Asio/Beast, JSON, no retries)
- [x] Phase 13 — online operation end to end (**milestone #2**)
- [x] Phase 14 — `TransactionService`: persist first, send second
- [x] Phase 15 — offline operation: `NetworkError` → `PENDING_SYNC` (**milestone #3**)
- [ ] Background sync of pending transactions, retries with backoff
