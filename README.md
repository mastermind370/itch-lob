# itch-lob

A low-latency order book and matching engine in C++20, built by replaying real NASDAQ TotalView-ITCH 5.0 market data.

> Work in progress. Stages 1–6 of 7 are done. The full benchmark report is in [`results/REPORT.md`](results/REPORT.md).

## Status

| Stage | What | Status |
|---|---|---|
| 1 | Zero-copy ITCH 5.0 parser, tests, tools | ✅ |
| 2 | Baseline `std::map` order book + per-message correctness check | ✅ |
| 3 | Fast order book (flat price ladder, preallocated order map) + latency percentiles | ✅ |
| 4 | Price-time matching engine + randomized differential test | ✅ |
| 5 | Lock-free SPSC pipeline (parser thread → book thread) | ✅ |
| 6 | Benchmark report: latency histograms, ablation table, counters | ✅ |
| 7 | Architecture diagram, final results | — |

## Build

Requires CMake ≥ 3.20 and a C++20 compiler (Apple clang 15+, clang 16+, gcc 12+).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

GoogleTest is downloaded automatically when it isn't installed.

## Data

```bash
scripts/fetch_sample.sh 12302019 1.5   # streams NASDAQ's sample day, keeps a 1.5 GB slice
```

Files come from `https://emi.nasdaq.com/ITCH/Nasdaq ITCH/`. Each file is a sequence of `[u16 big-endian length][message]` frames.

You don't need any download for a deterministic synthetic feed with a realistic message mix:

```bash
./build/gen_synthetic data/synthetic_20m.itch --messages 20000000 --stocks 256
```

## Stage 1: parser

```bash
./build/itch_stats data/12302019.slice.itch --reps 5 --json results/parse.json
```

Design:

- **Zero-copy.** The file is `mmap`ed. Integer fields are decoded with one unaligned load + byte swap, and symbol/MPID fields are `std::string_view`s into the mapped buffer. No allocation and no copying.
- **Static dispatch.** Handlers are plain structs that derive from `itch::NullHandler`. `parse_frames<H>` switches on the type byte and calls the handler directly, so the compiler inlines the callbacks, and unused message types cost nothing.
- **Defensive framing.** Each known type's length is checked against the spec table. A truncated trailing frame returns `Truncated` together with the byte offset to resume from, which also makes the parser usable on a socket stream.

Results (fill in from `results/parse.json`):

| Input | Messages | Throughput | ns/msg | Machine |
|---|---|---|---|---|
| NASDAQ 12302019, 1.5 GB slice | 48.06M | 234 M msgs/s (7.3 GB/s) | 4.3 | MacBook, Apple clang 17 |
| Synthetic 20M | 20.0M | 88 M msgs/s | 11.3 | 2-vCPU x86 cloud VM, clang 18 (dev sanity check only) |

## Stage 2: baseline order book

```bash
./build/book_replay data/12302019.slice.itch --book map --check   # verify every message
./build/book_replay data/12302019.slice.itch --book map --reps 3  # time it -> results/baseline.json
```

- `lob::MapBook`: one book per stock, made of two `std::map<Price, Level>` (bids sorted high to low, asks low to high). Each level holds its total shares and order count.
- `lob::BookBuilder<Book>`: the ITCH handler. It keeps an order-ID → resting-order map in a `std::unordered_map` and applies A/F/E/C/X/D/U to the right stock's book. Any book with the same interface plugs in, so Stage 3's fast book can be checked against this one.
- `lob::CheckedReplay`: correctness check. After **every** order message, it compares the shares and order count of each changed price level with an independently maintained level-sum table. At the end, it rebuilds every level from the live orders and compares in both directions. It also counts feed anomalies: duplicate IDs, unknown IDs, and executions larger than the shares left. A unit test shows the check catches a book that drops one share per add.

| Input | Messages | Check | Throughput | ns/msg | Machine |
|---|---|---|---|---|---|
| NASDAQ 12302019, 1.5 GB slice | 48.06M | passed (50.9M level checks, 735K-level audit, 0 mismatches, 0 anomalies) | 6.2 M msgs/s | 161 | MacBook, Apple clang 17 |
| Synthetic 20M, 256 stocks | 20.0M | passed (21.4M level checks, 0 mismatches) | 5.6 M msgs/s | 179 | 2-vCPU x86 cloud VM (dev sanity check only) |

## Stage 3: fast order book

```bash
./build/book_replay data/12302019.slice.itch --book fast --check  # same checks + best bid/ask vs std::map book
./build/book_replay data/12302019.slice.itch --reps 5             # map vs fast -> results/stage3.json
./build/book_replay data/12302019.slice.itch --latency            # per-message p50..p99.99 -> results/latency_*.json
```

- **Flat price ladder** (`lob::FastBook`). Each stock gets a 2048-tick array centered on its first price: ±$10.24 at penny ticks, with $0.0001 ticks below $1. Looking up a price takes one subtraction and one constant division. A 64-bit bitmap per side finds the next best level with `clz`/`ctz` when the best level empties. Prices outside the window, or off the tick grid, go to a `std::map` overflow, so the book is always exact. `overflow_adds` reports how often that happens. Pre-market stub quotes can put a stock's first order far from where it actually trades, so the window **recenters on the touch**: after every 256 overflow adds, the book checks whether its best bid or ask has left the window, and if so moves every level into the new window. This is rare and is counted in `recenters`. The `book_replay` output lists the 10 stocks with the most overflow.
- **Flat order map** (`lob::FlatOrderMap`). Open addressing with linear probing and Fibonacci hashing over one preallocated array of 4M slots (96 MB). Deletion is backward-shift, so there are no tombstones and lookups don't slow down over a day. `order_map_grows` reports any resize.
- **No heap allocation on the hot path.** Each stock's ladder (~33 KB) is allocated on its first add, once per stock. The only per-message allocations are overflow-map inserts, which are counted.
- **Shared builder.** `BookBuilder<Book, OrderMap>` is the same code for both books, so the speedup comes from the data structures alone.
- **Stricter checker.** Besides the Stage 2 checks, it compares the touched stock's best bid and ask, price and size, with a reference `std::map` book after every message. Unit tests show it catches a book with a stale best bid.
- **Latency.** Each message is timed alone with the CPU counter: `cntvct_el0` on arm64, `rdtscp` on x86. On Apple Silicon that counter ticks at 24 MHz, so single-message samples are multiples of 41.7 ns. That makes p50 coarse; p99 and p99.9 are still meaningful. Every sample includes the timer overhead, which the tool reports, so mean per-message latency comes out higher than throughput ns/msg.

| Input | Book | ns/msg | Speedup | p50 | p99 | p99.9 | Machine |
|---|---|---|---|---|---|---|---|
| NASDAQ 12302019, 1.5 GB slice | map | 167 | 1.0× | 125 ns | 833 ns | 1167 ns | MacBook |
| NASDAQ 12302019, 1.5 GB slice | fast, before recentering | 69 | 2.40× | 125 ns | 459 ns | 792 ns | MacBook |
| NASDAQ 12302019, 1.5 GB slice | fast | — | 3.15× | 125 ns | 333 ns | 708 ns | MacBook |
| Synthetic 20M, 256 stocks | map | 172 | 1.0× | 286 ns | 741 ns | 2376 ns | 2-vCPU x86 cloud VM (sanity check only) |
| Synthetic 20M, 256 stocks | fast | 80 | 2.1× | 163 ns | 431 ns | 556 ns | 2-vCPU x86 cloud VM (sanity check only) |

## Stage 4: matching engine

```bash
./build/engine_bench   # 10 seeds x 1M random orders diffed, then throughput + latency -> results/engine.json
```

ITCH data is already matched, so the engine runs on its own order flow: `engine::OrderFlow` generates deterministic random limit, market and cancel orders around a drifting mid. About 15% of limits are priced through the touch, and some cancels target orders that have already filled.

- **Semantics.** Price-time priority. An incoming order trades at the resting order's price: best price first, then oldest first within a price. A limit's remainder rests at the back of its level's queue, a market order's remainder is cancelled, and cancel removes a resting order. Rejects: duplicate id, unknown id, price 0 or outside the band, quantity 0.
- **`engine::RefEngine`.** `std::map<price, std::deque<order>>`, written to be obviously correct.
- **`engine::FastEngine`.** A flat ladder with one level per tick. Each level is an **intrusive doubly-linked FIFO** of nodes from a preallocated pool, so adding at the back, filling from the front and cancelling from the middle are all O(1). A **two-level bitmap** (64-bit words plus a summary bit per word) finds the next price in a few word scans across a 65,536-tick band. Order id → node uses the same flat open-addressing map as Stage 3, sized at 2× the pool so it never grows. Nothing is allocated after construction.
- **Cache sizing matters.** The default pool holds 65,536 resting orders per symbol. Sizing for 1M orders made the id map 32 MB and nearly doubled the cost per order: 23 ns/order at 16K capacity, 27 at 64K and 41 at 1M (x86 VM).
- **Differential test.** Every order goes to both engines. After every order it compares the result, the exact fill sequence (taker, maker, price, qty), both best quotes and the open-order count. It also checks invariants: the book never crosses after matching, and limit-order shares are conserved. Every 10K orders it compares both full books, including the order of every queue. A mutation test that fills the newest order first instead of the oldest is caught on the first affected order.

| Machine | Diff test | Orders/s | ns/order | Fills/s | Speedup vs ref | p50 | p99 | p99.9 |
|---|---|---|---|---|---|---|---|---|
| MacBook, Apple clang 17 | 10/10 seeds × 1M passed (2.44M fills) | 87.3M | 11.5 | 15.1M | 4.54× | < 41 ns* | < 41 ns* | 84 ns |
| 2-vCPU x86 cloud VM (sanity check only) | 4/4 seeds × 1M passed | 39.9M | 25.1 | 6.9M | 3.96× | 60 ns | 146 ns | 260 ns |

\* The Mac's counter advances in 41 ns steps and a typical order takes about 11 ns, so p50 and p99 fall below one step. p99.9 is the first percentile above it, and p99.99 is 125 ns.

## Stage 5: lock-free pipeline

```bash
./build/pipeline_bench data/12302019.slice.itch --reps 3   # -> results/pipeline.json
```

The parser thread decodes each frame and pushes a 40-byte `Event` for every order message into a single-producer single-consumer ring buffer. The book thread pops events and applies them to the Stage 3 fast book. Every run's final books are hashed and checked against the single-thread run.

- **`pipeline::SpscQueue`.** A power-of-two ring with 64-bit monotonically increasing indices, using only acquire/release (no CAS). Three optimizations can each be switched off for the ablation:
  - **Cache-line padding.** Producer state, consumer state and slots each get their own 128-byte line, which is the line size on Apple Silicon and also covers Intel's paired-line prefetch.
  - **Cached indices.** Each side keeps a private copy of the other side's index and reads the shared atomic only when the queue looks full or empty.
  - **Batched head publication.** The consumer publishes its read position every 32 pops, or immediately when it finds the queue empty. Without this, a full queue makes the producer re-read `head` after every pop, so that line moves between cores once per message. Latency doesn't change, because the producer still publishes every push immediately.
- **Thread placement.** On Linux the threads are pinned to CPUs 0 and 1. macOS has no hard affinity on Apple Silicon, so both threads ask for the user-interactive QoS class, which the scheduler keeps on performance cores.
- **Latency.** Each event carries the tick at which the producer started parsing it, and the consumer records the time after applying it. That covers parse, enqueue, core-to-core transfer, dequeue and book update. The producer is paced to a fixed rate (by default half the measured pipeline throughput), so the numbers measure the path rather than a backlog. The tool warns if the producer falls behind.
- **Tests.** Two-thread ordering and sum checks on 8-slot rings for every variant. Pipeline vs single-thread digest on a synthetic feed, including a 2-slot unpadded ring. ThreadSanitizer is clean.

| Machine | Inline | Pipeline | No head batching | No padding | Naive | Latency, inline p50 / p99 | Latency, pipeline p50 / p99 |
|---|---|---|---|---|---|---|---|
| MacBook, Apple clang 17 | 19.0M msgs/s | 14.5M | 10.6M | 9.8M | 9.8M | 125 / 334 ns | 208 / 792 ns at 4M msgs/s (p99.9 41 µs‡) |
| 2-vCPU x86 KVM VM (sanity check only) | 12.4M msgs/s | 8.2M | 7.1M | 5.2M | 5.2M | 168 / 449 ns | 594 / 239,000 ns at 1M msgs/s** |

\*\* On this VM the two-thread runs are slower than one thread, and the tail is dominated by hypervisor preemption: the producer itself misses about 4% of its send times even at 1M msgs/s. The padding and batching ablation still shows the expected ordering.

‡ The Mac's paced run had the producer late for 0.06% of sends, so the latency is clean, and the queue adds 83 ns at p50. The p99.9 tail comes from the OS scheduler, which on macOS can't be stopped from preempting a spinning thread. Stage 6's counters count these preemptions.

## Stage 6: benchmark report

```bash
scripts/run_all.sh                      # every test, check and benchmark (10-15 min), then the report
python3 scripts/report.py               # rebuild results/REPORT.md + results/plots/*.svg from existing JSON
```

- **`ablation`** adds one book optimization at a time: `std::map` baseline → flat order map → flat price ladder → recentering, plus the ladder alone. Every variant is the same `BookBuilder<Book, OrderMap>` code. Runs are **interleaved** round-robin, so drift in clock speed or temperature hits every variant equally. It reports the median, min–max range and speedup vs the baseline and vs the previous step, and checks that every variant builds identical final books.
- **Counters** (`bench/counters.hpp`). On Linux, `perf_event_open` gives IPC, branch misses, L1d and LLC misses per message. Everywhere, `getrusage` gives page faults and voluntary/involuntary context switches. macOS doesn't expose hardware counters without Instruments or root, but context switches are what explain its latency tail.
- **Repeated runs everywhere.** `pipeline_bench` interleaves all five queue variants and reports median [min..max]. `engine_bench` reports the spread and a pool-capacity sweep showing the id map falling out of cache.
- **Latency histograms.** Every latency run saves its full histogram, and `report.py` draws HdrHistogram-style percentile plots (log latency vs p50…p99.999). It uses only the standard library and writes plain SVG with hover tooltips.
- **`results/REPORT.md`** has correctness checks, the ablation table, latency tables and plots for the book, engine and pipeline, and the headline numbers for the resume bullet.

## Layout

```
include/itch/   endian.hpp  messages.hpp  parser.hpp  encoder.hpp  synthetic.hpp  mapped_file.hpp
include/lob/    map_book.hpp  fast_book.hpp  order_map.hpp  book_builder.hpp  checker.hpp
include/engine/ types.hpp  ref_engine.hpp  fast_engine.hpp  order_flow.hpp  diff.hpp
include/pipeline/ spsc_queue.hpp  pipeline.hpp  cpu.hpp
include/bench/  latency.hpp  counters.hpp  stats.hpp
tools/          itch_stats  itch_slice  gen_synthetic  book_replay  engine_bench  pipeline_bench  ablation
tests/          GoogleTest unit tests
scripts/        fetch_sample.sh  run_all.sh  report.py
results/        JSON written by the tools; README tables are filled from these
```
