# Market Data Replay — C++20

Personal C++ systems project for replaying synthetic level-2 market-data updates inside a bounded price window. It is an offline simulator, not a live exchange feed or trading system.

## Architecture and scope

1. **Input and validation:** A strict CSV parser reads absolute price-level updates (`sequence,side,price_tick,quantity`). Quantity zero removes a level. Duplicate/stale updates are ignored and counted; sequence gaps stop replay unless a usable snapshot is supplied. Prices outside the configured window are rejected.
2. **Book and recovery:** The book holds at most 4,096 price ticks per side in arrays. A two-level bitmap marks occupied ticks, so finding the best bid or ask uses bit operations instead of scanning all levels. A complete snapshot is validated in a replacement book, then swapped into place; malformed snapshots leave the current book unchanged.
3. **Evaluation:** A deterministic synthetic workload runs the bitmap implementation and a `std::map` baseline over the same absolute updates. Both compute a rolling top-of-book checksum. The benchmark prints throughput plus p50/p99 time per update **including** best-price lookup and timing overhead. These are local measurements, not trading-system latency claims.

The fixed tick window is a deliberate tradeoff: it makes memory predictable and updates simple, but the book rejects prices outside the configured range. The CSV reader loads the full file into memory. It does not implement order matching, market data protocols, network transport, transaction recovery, or production operations.

## Build and run

Requires a C++20 compiler and `make`; CMake 3.20+ is an alternative. No external libraries or market data.

```sh
make
make test
make demo
make bench
```

With CMake, use `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`, `cmake --build build`, and `ctest --test-dir build --output-on-failure`.

The sample contains sequence 1, 2, a duplicate 2, then 4 and 5. The sequence-3 snapshot bridges the gap. The expected final top is bid `100101@5`, ask `100199@7`.

## CSV formats

Updates: `sequence,B|A,price_tick,absolute_quantity`. Snapshot: `seq,<sequence>` followed by `B|A,price_tick,positive_quantity` rows. Blank lines and lines starting with `#` are ignored. The default window is ticks `100000..104095`; adjust with `--min-tick` and `--levels` (maximum 4,096).

## Acceptance checks

- Best bid/ask across bitmap-word boundaries; insert, replace, delete, and empty side.
- Duplicate, gap, and out-of-window updates do not corrupt book state.
- Invalid/old snapshots fail without modifying state; a valid snapshot bridges a missing sequence.
- Parsers reject malformed side, quantity, and snapshot header.
- Benchmark verifies matching checksums and final top against the `std::map` baseline.

## Verified results

Verified on 27 October 2026 with Apple clang 21.0.0 (arm64), Darwin 27.0.0, Mac14,2 (8 logical CPUs), `-O3 -DNDEBUG`:

```text
make test: passed 35 checks
make demo: applied=4 skipped_duplicate=1 recoveries=1 last_seq=5 best_bid=100101@5 best_ask=100199@7
make bench: events=100000, levels=4096, seed=42
bitmap: 10,982,925 events/s; p50 42 ns; p99 84 ns
std::map: 6,417,335 events/s; p50 125 ns; p99 250 ns
checksum_match=yes (10346526397604972815)
```

The benchmark includes one absolute level update and best-price read per event, plus timer and checksum overhead. Bitmap throughput was about 1.71 times the baseline in this run. Performance varies with compiler, hardware, workload, and measurement order. A separate AddressSanitizer/UndefinedBehaviorSanitizer build of the 35 checks also passed. CMake support is provided but was not tested locally because CMake was unavailable.
