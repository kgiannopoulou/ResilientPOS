# ResilientPOS — demo

Short on time? `./build/resilient-sim` runs a whole shop through an outage in one
command and ends with the verification; section 8 below walks through it. The full
walkthrough takes 5–7 minutes.

Three terminals side by side: **server**, **till**, **till log**. Start from
clean databases so the counts below match:

```bash
rm -f data/*.db* data/*.log
```

| Terminal | Command |
|---|---|
| A — server | `./build/resilient-server` |
| B — till   | `./build/resilient-pos --metrics-port 9100` |
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

**B:** `4` (Network status) — `OFFLINE`, worker `Backing off: retry #3 in 2s`.
**C:** `Sync retry scheduled attempt=1 delay_ms=613 ceiling_ms=1000`, then ceilings
of 2000, 4000 ...

> "The worker isn't polling. It's on a timed condition-variable wait for an
> exponential backoff with full jitter: the ceiling goes 1, 2, 4, 8, 16, then
> 30 seconds, and each wait is random under it. Without that, a hundred tills
> that lost the server together would all retry together, right when it comes
> back. The randomness is injected, so the tests stay deterministic."

## 3. Crash recovery (30 s)

**B:** close the terminal window (or `6`), start `./build/resilient-pos --metrics-port 9100` again.
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

## 6. Same ID, different payload (45 s)

**A:** press `1` (normal). Then, from a fourth terminal:

```bash
curl -X POST localhost:8080/transactions -d '{"transaction_id":"abc-123","amount_cents":1000}'
# {"status":"ACCEPTED"}
curl -X POST localhost:8080/transactions -d '{"transaction_id":"abc-123","amount_cents":1000}'
# {"status":"ALREADY_PROCESSED"}
curl -X POST localhost:8080/transactions -d '{"transaction_id":"abc-123","amount_cents":10000}'
# 409 {"status":"IDEMPOTENCY_CONFLICT","error":"transaction_id was already used with a different payload"}
```

> "An ID alone isn't enough. If the second request were treated as a retry, the
> till would think €100 was stored while the server holds €10, and nobody would
> ever find out. So the server stores a hash of the payload: same ID and same hash
> is a retry, same ID and a different hash is refused."

## 7. A sale that can never succeed (60 s)

**A:** press `8` — *Poison (422 permanent refusal)*. **B:** ring up `Bagel 2.50 1`:

```
[SERVER] Rejected: HTTP 422: ...
Status: DEAD_LETTER
The server refused this sale, so it will not be retried.
```

**B:** `5` (Dead letter queue) shows the sale with its reason. **A:** press `1`.
**B:** `5` again, then `1` to retry it. It goes back through the queue and syncs.

> "Failures are classified by what they say about the sale. Being unreachable says
> nothing, so that retries forever and never uses up the retry budget: a till can
> be offline for days. A 500 might be about this sale, so it gets ten tries. A 422
> is a verdict, so retrying is pointless. The sale is never deleted, it waits on
> disk for a person."

**Then look at the numbers:**

```bash
curl localhost:9100/metrics | grep -E '^(transactions|sync_failures|retry)'
curl localhost:8080/metrics | grep -E '^(server_|idempotency)'
curl localhost:8080/ready
```

> "A till that's offline still answers `/ready`, because all it needs to take a
> sale is its own database."

## 8. A whole shop (60 s)

**A:** `Ctrl+C`. Then `./build/resilient-sim --terminals 5`.

> "This is the same code with no fakes: five terminals, each with its own SQLite
> file and sync worker, talking TCP to a multi-threaded server. One terminal loses
> its link and the others don't notice. Then the server really stops, everyone keeps
> selling, and each backs off on its own random schedule. The server comes back but
> drops replies after committing, so terminals resend sales it already has. Then
> someone replays a known ID with a new amount. At the end it compares every sale
> against the server, by ID and amount, and exits non-zero if anything is off, which
> is why CI runs it on every push."

## 9. Close (30 s)

**B:** `6` → `Stopping sync worker... done.`

> "Shutdown is RAII: `request_stop()` wakes the worker's wait, the in-flight send
> finishes, the `jthread` joins, then SQLite closes — destruction order in
> `main()` is the shutdown order. CI runs the whole suite under ThreadSanitizer."


