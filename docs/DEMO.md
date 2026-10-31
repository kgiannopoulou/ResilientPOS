# ResilientPOS — 3–5 minute demo

Three terminals side by side: **server**, **till**, **till log**. Start from
clean databases so the counts below match:

```bash
rm -f data/*.db* data/*.log
```

| Terminal | Command |
|---|---|
| A — server | `./build/resilient-server` |
| B — till   | `./build/resilient-pos` |
| C — log    | `tail -f data/pos.log` |

## 1. Online sale (30 s)

**B:** `1` → `Coffee` → `3.50` → `2`

```
[LOCAL] Transaction persisted
[NETWORK] Sending...
[SERVER] Accepted
Status: SYNCED
```

> "The sale is written to SQLite *before* any network call. Only then is it sent."

## 2. Server goes down, the till keeps selling (60 s)

**A:** `Ctrl+C`.
**B:** ring up three sales (`Tea 2.00 1`, `Cake 4.20 1`, `Juice 3.00 2`).

```
[NETWORK] Server unavailable
Status: PENDING_SYNC
Transaction safely stored locally.
POS can continue operating.
```

**B:** `4` (Network status) — `OFFLINE`, worker `Backing off: retry #3 in 4s`.
**C:** `Sync retry scheduled attempt=1 delay_ms=1000`, then 2000, 4000 ...

> "The worker isn't polling. It's on a timed condition-variable wait for an
> exponential backoff: 1, 2, 4, 8, 16, then 30 seconds."

## 3. Crash recovery (30 s)

**B:** close the terminal window (or `5`), start `./build/resilient-pos` again.
Main screen: `Pending transactions: 3`.

> "The queue *is* the database. Restarting loses nothing."

## 4. Server comes back, the queue drains by itself (30 s)

**A:** `./build/resilient-server`. Within one backoff period:

**C:**
```
[INFO] Transaction synced id=... result=ACCEPTED   (x3)
[INFO] Sync pass finished synced=3 already_processed=0 failed=0 pending=0
```

**B:** press Enter — `Network: ONLINE`, `Pending transactions: 0`.

## 5. The killer case: the response is lost (90 s)

**A:** press `6` — *Drop after processing (stored, no answer)*.
**B:** ring up `Sandwich 5.50 1`.

**A** shows:
```
[INFO] Transaction accepted id=03ae7ba4 amount=550
[WARN] Simulated drop after processing: COMMITTED, response dropped
```
**B** shows `[NETWORK] Server unavailable` and `Status: PENDING_SYNC`.

> "Now the two sides disagree. The server has the sale; the till thinks it
> failed. The till can't know which it was — dropped before or after the
> commit — so it always retries with the same transaction ID."

**A:** press `1` (normal). **B:** `3` (Force synchronization):

```
[SYNC] 03ae7ba4... -> ALREADY_PROCESSED -> SYNCED (server had it; no duplicate)
```

**A:** `[INFO] Duplicate transaction id=03ae7ba4 result=ALREADY_PROCESSED (not stored again)`

> "The server's primary key on the transaction ID makes the resend a no-op.
> Customer charged once, stored once, till ends SYNCED. That sequence is the
> point of the project, and it's pinned by the integration test
> `LostResponseDoesNotCauseDuplicateTransaction`."

## 6. Close (30 s)

**B:** `5` → `Stopping sync worker... done.`

> "Shutdown is RAII: `request_stop()` wakes the worker's wait, the in-flight send
> finishes, the `jthread` joins, then SQLite closes — destruction order in
> `main()` is the shutdown order. CI runs the whole suite under ThreadSanitizer."

## Likely questions

- **Why `CREATED` while sending, not `PENDING_SYNC`?** So the background worker,
  which only claims `PENDING_SYNC` rows, can never send the same sale at the same
  time as the UI thread. A `CREATED` row left by a crash is requeued at startup.
- **Why stop the pass at the first network error?** If the server is unreachable
  for one sale it's unreachable for all; each failed attempt can take seconds.
- **What about many tills?** UUIDs are generated offline, so no coordination;
  the server's idempotency is enforced by the database, not by its single thread.
  Next steps would be jitter on the backoff and a dead-letter state for sales the
  server permanently rejects.
