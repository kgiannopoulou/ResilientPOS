# ResilientPOS

An **offline-first Point-of-Sale engine** written in modern C++20.
The till keeps taking sales when the network drops, persists everything locally,
and synchronises with the server once connectivity returns.

> Status: early development — skeleton + core domain model (`Money`, `TransactionStatus`, `Transaction`).

![CI](https://github.com/kgiannopoulou/ResilientPOS/actions/workflows/ci.yml/badge.svg)

## Build

```bash
cmake -S . -B build
cmake --build build
./build/resilient-pos
```

Expected output:

```
=========== ResilientPOS ===========
Coffee:     €3.50
2x Coffee:  €7.00

Transaction 3f1c9a52-7d0e-4b6a-9c21-5e8f0d4a7b13
  Amount:   €7.00
  Status:   CREATED
  Attempts: 0
```

(The transaction ID is a fresh random UUID on every run.)

### Tests

Unit tests use [Catch2 v3](https://github.com/catchorg/Catch2), fetched automatically by CMake.

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

```
Money
  ↓
Transaction ── id (UUID v4), createdAt, syncAttempts
  ↓
TransactionStatus
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
- [ ] Local persistence (SQLite)
- [ ] Networking + sync with the server
- [ ] Offline queue, retries and conflict handling
