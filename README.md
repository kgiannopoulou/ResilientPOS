# ResilientPOS

An **offline-first Point-of-Sale engine** written in modern C++20.
The till keeps taking sales when the network drops, persists everything locally,
and synchronises with the server once connectivity returns.

> Status: early development — project skeleton.

## Build

```bash
cmake -S . -B build
cmake --build build
./build/resilient-pos
```

Expected output:

```
=========== ResilientPOS ===========
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
