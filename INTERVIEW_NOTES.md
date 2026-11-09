# Interview notes: market-data-replay

## 1. What it is

A personal C++20 project that replays a synthetic, exchange-style binary market
data feed and rebuilds an order-by-order book from it. The feed comes on two
redundant lines (A and B) that each lose, duplicate and reorder packets. The
replayer merges the two lines, notices when data is truly missing, and recovers
from a validated snapshot. A differential fuzzer and checks against a "truth"
digest show that the final book is exactly right.

## 2. How it works (and why)

**Data flow:** `mdr_gen` → `line_a.bin`, `line_b.bin`, `snapshots.bin`,
`truth.txt` → memory-mapped → arbiter/decoder thread → SPSC ring → book thread
→ final digest compared with truth.

- **Fixed 32-byte little-endian messages, packets with a 24-byte header.**
  Message *i* in a packet has sequence `first_seq + i`, as in MoldUDP64.
  - *Why:* fixed size means no parsing state and bounds checks are trivial.
  - Reads and writes go byte-by-byte, so the code doesn't depend on the CPU's
    byte order; compilers usually merge those into single loads, though I didn't check the assembly.
  - A reserved field must be zero, so some corruption gets caught at decode.
- **Order-by-order book, not just price levels.** Adds, cancels, deletes,
  executes and modifies all refer to an order id.
  - Each price level keeps a FIFO queue of orders as a doubly linked list
    through a pool, so removing from the middle is O(1).
  - A custom open-addressing hash table (linear probing, backward-shift delete,
    no tombstones) maps order id → pool slot.
  - Priority rules: reducing size keeps your place; a price change or size
    increase sends you to the back.
- **The price-level index is a template parameter** (bitmap / `std::map` /
  sorted vector).
  - *Why:* the benchmark then changes exactly one thing; the pool, hash table
    and rules are shared.
- **Bitmap index:** a 4,096-tick window per instrument and side.
  - Level lookup is `levels[price - base]`.
  - Best price comes from a 64-bit summary word (which 64-tick block is
    non-empty), then the word for that block, using `clz` for bids and `ctz`
    for asks.
  - *Trade-off:* memory is fixed (4,096 × 24 B per side) and prices outside the
    window are rejected.
- **A/B arbitration:**
  - Merge both captures by arrival time; first copy of each sequence range wins.
  - Packets that arrive ahead of a hole are buffered (`std::map` keyed by
    sequence), because the other line usually fills the hole a few µs later.
  - A hole open for more than 50 µs of feed time, or a buffer over its limit,
    counts as a real loss.
  - Recovery then uses the earliest snapshot that covers the hole and passes
    validation: FNV-1a checksum, sane fields, unique ids.
  - Snapshot records are stored in FIFO order, so rebuilding preserves queue
    priority.
  - The snapshot is rebuilt into a fresh book and swapped in only if every
    record applies.
- **Pipeline:** a bounded SPSC ring of 32-byte events.
  - Head and tail sit on separate 128-byte cache lines (the M2 line size).
  - Each side caches the other's index.
  - The producer publishes its tail once every 64 events (every event when
    paced).
  - Recovery is sent through the ring as a control event, so only the book
    thread ever touches the book.
  - Waits spin, then yield, because pure spinning collapsed when the shared
    machine was overloaded.
- **Testing approach:**
  - A deliberately dumb reference book (`unordered_map` + `std::map` of
    vectors) is the oracle.
  - The fuzzer compares statuses, best bid/ask, and a full digest of every
    level and order.
  - Generated sessions with random faults must end with digest == truth.
- **Measurement approach:**
  - Release `-O3`, 5 repetitions per throughput number with the run order
    rotated, and min/median/max reported.
  - Every timed run also checks the digest, so a fast-but-wrong result can't
    sneak in.

## 3. Key results (all measured by me, M2 MacBook Air, Apple clang 21, `-O3 -DNDEBUG`)

Workload: 3M messages, 8 instruments, ~15.8k resting orders, ~79 levels per
side.

- **Book only:** bitmap **42.2M msg/s** (41.3–42.7, 5 runs) vs `std::map`
  **20.1M** (**2.1x**) vs sorted vector 24.7M (1.7x).
- **Full replay:** arbitration + decode + apply was 29.5M msg/s in one thread
  and **29.1M msg/s** in the 2-thread pipeline, so splitting into two threads
  gave **no speedup**.
- **Paced at 1M msg/s:** decoder push → book applied, p50 **125 ns** (bitmap).
  - Without recoveries: p99.9 0.6–13 µs in 8 of 9 runs.
  - With 3 snapshot recoveries: p99.9 0.26–0.43 ms; each restore stalls the
    book thread 0.43–0.56 ms.
- **Recovery behaviour:** 3 packets were lost on both lines.
  - Line B filled 4,820 of line A's holes.
  - 3 recoveries gave an exact final book.
  - Without snapshots, 13 skipped messages caused 9 apply errors and a wrong
    book.
- **Correctness:**
  - 3.2M fuzz events (430k invalid on purpose) × 3 book variants, 0 mismatches.
  - 40 random-fault sessions × 4 replays, 0 mismatches.
  - 120 unit checks.
  - ASan+UBSan and TSan clean.

**What they do NOT show**
- Not real market data. The order flow is a toy model, and real data might
  favour a different structure.
- Not network latency. Arrival times are fields in a file, and there's no
  kernel, NIC or UDP.
- Per-message timings are limited by a 41.7 ns timer tick.
  - Paced latency is stamped when the decoder pushes, so decoder-side stalls
    are hidden.
- I didn't measure cache misses (no hardware counters without root on macOS).
  The "why" explanations are hypotheses.
- One shared laptop with no core pinning. Tails vary a lot between runs.
- Recovery is idealised: snapshots sit in a file, even ones taken after the
  hole.

## 4. Two-minute answer (STAR)

### Situation
I'd read that exchanges send market data as a stream of tiny binary messages
over UDP, on two redundant lines, because packets get lost. I had a small CSV
project that replayed price levels, and I wanted to know what this really looks
like. What happens at a million messages a second, and what happens when you
lose one packet? This was a personal project. I built it with AI
pair-programming help, mostly for design discussion and review. I ran and
checked every test and number myself.

### Task
I wanted three things:
1. A replayer that rebuilds an exact order-by-order book from a binary feed.
2. Handling for two lossy, reordering lines, with recovery from snapshots when
   both lose the same data.
3. Honest measurements of which data structure is fastest for the book, and
   whether multithreading helps.

The rule I set myself: every number must come from a run I can reproduce.

### Action
- **Format and generator.** I defined a fixed 32-byte little-endian message
  format and wrote a generator that simulates order flow for 8 instruments. It
  produces two captures with loss, duplicates, late packets and outages, plus
  snapshots and the true final book digest.
- **Book.** Orders sit in per-price FIFO queues, with a custom hash table from
  order id to order. I made the price-level index swappable (a two-level
  bitmap, `std::map`, or a sorted vector) so I could compare them fairly.
- **Arbiter.** The arbiter merges the lines, buffers out-of-order packets, and
  after a timeout recovers from the nearest snapshot that passes a checksum.
- **Testing.** Apple clang has no libFuzzer, so I wrote a seeded differential
  fuzzer. It checks all three books against a deliberately simple reference
  book on every event. I also ran ASan, UBSan and TSan.
- **Pipeline.** I split decoding and book-building into two threads with a
  lock-free single-producer/single-consumer ring.

### Result
- **Speed.** The bitmap book did 42 million messages a second, about 2.1x
  `std::map`, and the full replay including arbitration ran at about 29 million.
- **Correctness.** Three packets were lost on both lines. The replayer
  recovered and the final book matched the truth exactly. With recovery turned
  off, those 13 lost messages broke the book permanently.
- **Two threads were no faster than one.** I think that's cross-core cache
  traffic, but I haven't proven it.
- **Tail latency.** At a steady 1M msg/s the median was 125 nanoseconds. Each
  snapshot restore was half a millisecond of stall, and that showed up in the
  p99.9.
- **What I learned.** Measuring honestly is harder than making things fast.
  I learned to report spreads and to write down what my numbers don't show.

## 5. Likely follow-up questions

1. **Why is the bitmap faster than `std::map`?**
   Lookup is an array index and best price is two bit-scans. A map lookup is
   ~6–7 dependent pointer loads plus a heap allocation when a level is created.
   I believe it's mostly cache misses and allocation, but I didn't measure
   misses. The cost is fixed memory and a hard price window.

2. **Why didn't the pipeline speed things up?**
   Arbitration and decode are only ~10 ns/msg. The book thread reads events
   that were just written on another core, so it pays coherence traffic. I
   batched tail publication, and it still only tied the single thread. Next
   steps would be bigger batches, having the book thread decode itself from
   raw packet pointers, or measuring with Instruments.

3. **How do you know the book is correct?**
   - A reference book with no clever data structures.
   - The fuzzer compares status codes and top of book on every event, and a
     full digest of every level and every order in queue order every 1,000
     events.
   - Sessions with random loss and corruption must end on the generator's
     truth digest.
   - Every benchmark run checks the digest too.

4. **Is the SPSC ring correct? What memory ordering do you use?**
   The producer writes the slot, then publishes the tail with a release store.
   The consumer reads the tail with an acquire load before reading the slot,
   and the same applies in reverse for the head. Each side caches the other's
   index and only reloads when the ring looks full or empty. A threaded order
   test, the pipeline tests with a tiny 64-slot ring, and TSan back it up, but
   that's testing, not a proof.

5. **What happens on a gap in a real system that you skipped?**
   A real handler would request a snapshot or replay, keep buffering live
   packets, then apply the buffer on top. Here snapshots sit in a file, so
   recovery is instant and can use a snapshot from "after" the hole. Single-line
   replay looks unrealistically good because of that.

6. **Why 50 µs for the gap timeout?**
   It's larger than line B's ~2 µs lag plus jitter and most late packets
   (3–30 µs), so arbitration fills ordinary holes. Setting it to 0 still gives
   a correct book, but each premature recovery jumps ahead to the next
   snapshot. In real life that would be a costly snapshot request.

7. **How did you measure latency, and what's wrong with it?**
   I read `cntvct_el0` directly, which ticks at 24 MHz (41.7 ns). Results go
   into a log-bucket histogram with 8 sub-buckets per power of two, and
   percentiles report the bucket's upper bound. The weaknesses:
   - coarse ticks
   - no core pinning on macOS
   - a shared machine
   - stamping at push hides decoder-side stalls (coordinated omission)

8. **What would you do next?**
   - Measure cache misses with Instruments.
   - Try decoding on the book thread directly from the mmap.
   - Add a realistic snapshot request/replay protocol.
   - Run on Linux with pinned, isolated cores to see what the tails do.

## 6. Things I should be able to explain cold

- [ ] The 32-byte message layout, and why the packet header carries the sequence number
- [ ] How the two-level bitmap finds the best bid (clz) vs best ask (ctz)
- [ ] Why the price window is 4,096 = 64 × 64, and what happens outside it
- [ ] Per-level FIFO with intrusive doubly linked lists; O(1) cancel from the middle
- [ ] Backward-shift deletion in linear probing (why there are no tombstones)
- [ ] Modify priority rules (size down keeps priority; price change or size up loses it)
- [ ] The arbiter state: `expected`, the buffer, gap timeout, recovery choice, stale packets after recovery
- [ ] Snapshot validation steps and why the restore is atomic (fresh book, then swap)
- [ ] SPSC ring: acquire/release pairs, cached indices, 128-byte lines on M2, batched publish
- [ ] Why the two-thread pipeline didn't win, and how I'd test the hypothesis
- [ ] Throughput methodology: 5 reps, rotated order, median plus min–max, digest checked every run
- [ ] Timer resolution (24 MHz) and why per-message numbers are only "under ~85 ns"
- [ ] Why p99.9 rises with recovery and why my count of delayed messages doesn't fully explain it
- [ ] What the differential fuzzer compares, and how often
- [ ] Exactly what this project is *not* (synthetic, offline, made-up protocol, idealised recovery)
