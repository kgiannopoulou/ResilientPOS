# ResilientPOS

An **offline-first Point-of-Sale engine** written in modern C++20.
The till keeps taking sales when the network drops, persists everything locally,
and synchronises with the server once connectivity returns.

> Status: early development — domain model, durable SQLite storage and a persistent POS CLI.
> Milestone #1 reached: **a POS crash/restart doesn't destroy pending transactions.**

![CI](https://github.com/kgiannopoulou/ResilientPOS/actions/workflows/ci.yml/badge.svg)

## Build

```bash
cmake -S . -B build
cmake --build build
./build/resilient-pos
```

The POS stores sales in `data/resilientpos.db` (pass another path as the first argument):

```
=========== ResilientPOS ===========
Database: data/resilientpos.db

1. New transaction
2. List pending
3. Exit

Choice: 1

Product: Coffee
Price cents: 350
Quantity: 2

2 x Coffee @ €3.50
Total: €7.00

Transaction created:
3dd01095-04c4-4bc1-ab5b-2033281193c7

Status: PENDING_SYNC
```

Exit, start it again, choose `2` — the sale is still there:

```
1 transaction(s) waiting to sync:

  3dd01095-04c4-4bc1-ab5b-2033281193c7  2026-10-28 08:39:53 UTC  €7.00  PENDING_SYNC

Pending total: €7.00
```

### Tests

Unit tests use [Catch2 v3](https://github.com/catchorg/Catch2); it and the
[SQLite amalgamation](https://www.sqlite.org/amalgamation.html) (3.46.1, SHA-256 pinned)
are fetched automatically by CMake — no system packages needed.

```bash
ctest --test-dir build --output-on-failure
```

Requirements: CMake ≥ 3.20 and a C++20 compiler (GCC 11+, Clang 14+, MSVC 2022+).

## Design notes

- **Money is integer cents.** `Money` wraps an `int64_t` count of cents — no floating
  point anywhere near currency. Addition is overflow-checked and throws
  `std::overflow_error` instead of silently wrapping.
- **Transactions are born offline.** Every sale gets a random version-4 UUID
  (`UuidGenerator`) on the till itself, so many tills can create sales without
  talking to the server and IDs still never collide.
- **Explicit sync lifecycle.** `TransactionStatus` models
  `CREATED → PENDING_SYNC → SYNCING → SYNCED` (or `FAILED`, then retried);
  `Transaction::syncAttempts` counts retries for later backoff logic.

- **Business logic depends on interfaces.** `TransactionRepository` and
  `ServerClient` are abstract; SQLite and (later) HTTP are implementation details
  behind them, so services can be tested against fakes.
- **The local database is the source of truth.** `SQLiteTransactionRepository`
  uses WAL mode with `synchronous=FULL`, so a sale is fsynced before `save()`
  returns. SQLite handles (`sqlite3*`, `sqlite3_stmt*`) are owned by RAII
  wrappers — no manual cleanup paths. Duplicate IDs are rejected by the
  `PRIMARY KEY`; unknown IDs in `updateStatus()` throw `StorageError`.
- **Save, then mark pending.** The CLI calls `save()` and then
  `updateStatus(PendingSync)`; a crash between the two leaves the sale on disk
  as `CREATED` rather than losing it.
- **Duplicates are success.** `SendResult::successful()` is true for both
  `Accepted` and `AlreadyProcessed`: if the server stored a sale but the ACK was
  lost, the retry comes back `AlreadyProcessed` and the till can still mark it
  `SYNCED`. This is what will make resending safe.

```
             apps/pos (CLI)              TransactionService (next)
                  │                         │             │
                  ▼                         ▼             ▼
        TransactionRepository  ◄────────────┘        ServerClient
                  ▲                                       ▲
                  │                                       │
     SQLiteTransactionRepository                  HttpServerClient (later)
                  │
           data/resilientpos.db

Transaction ── id (UUID v4), Money amount, TransactionStatus, createdAt, syncAttempts
```

### Schema

```sql
CREATE TABLE IF NOT EXISTS transactions (
    id TEXT PRIMARY KEY,
    amount_cents INTEGER NOT NULL,
    status TEXT NOT NULL,           -- CREATED, PENDING_SYNC, SYNCING, SYNCED, FAILED
    created_at INTEGER NOT NULL,    -- milliseconds since Unix epoch
    sync_attempts INTEGER NOT NULL DEFAULT 0
);
```

## Layout

```
include/resilientpos/   public headers (core, storage, network, service, sync, util)
src/                    implementation files, mirroring include/
apps/pos/               POS terminal executable
apps/server/            central sync server executable
tests/                  unit tests
data/                   local runtime data (ignored by git)
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
- [ ] `TransactionService` + sync with the server
- [ ] Offline queue, retries and conflict handling
