# itch-lob

A low-latency order book and matching engine in C++20, built by replaying real NASDAQ TotalView-ITCH 5.0 market data.

> Work in progress. Stages 1–2 of 7 are done: the parser and the baseline book.

## Status

| Stage | What | Status |
|---|---|---|
| 1 | Zero-copy ITCH 5.0 parser, tests, tools | ✅ |
| 2 | Baseline `std::map` order book + per-message correctness check | ✅ |
| 3 | Fast order book (flat price ladder, preallocated order map) | — |
| 4 | Price-time matching engine + randomized differential test | — |
| 5 | Lock-free SPSC pipeline (parser thread → engine thread) | — |
| 6 | Benchmark report: latency histograms, ablation table | — |
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
| NASDAQ 12302019, 1.5 GB slice | 48.06M | — M msgs/s | — | MacBook |
| Synthetic 20M | 20.0M | 88 M msgs/s | 11.3 | 2-vCPU x86 cloud VM, clang 18 (dev sanity check only) |

## Stage 2: baseline order book

```bash
./build/book_replay data/12302019.slice.itch --check   # verify every message
./build/book_replay data/12302019.slice.itch --reps 3  # time it -> results/baseline.json
```

- `lob::MapBook`: one book per stock, made of two `std::map<Price, Level>` (bids sorted high to low, asks low to high). Each level holds its total shares and order count.
- `lob::BookBuilder<Book>`: the ITCH handler. It keeps an order-ID → resting-order map in a `std::unordered_map` and applies A/F/E/C/X/D/U to the right stock's book. Any book with the same interface plugs in, so Stage 3's fast book can be checked against this one.
- `lob::CheckedReplay`: correctness check. After **every** order message, it compares the shares and order count of each changed price level with an independently maintained level-sum table. At the end, it rebuilds every level from the live orders and compares in both directions. It also counts feed anomalies: duplicate IDs, unknown IDs, and executions larger than the shares left. A unit test shows the check catches a book that drops one share per add.

| Input | Messages | Check | Throughput | ns/msg | Machine |
|---|---|---|---|---|---|
| NASDAQ 12302019, 1.5 GB slice | 48.06M | — | — M msgs/s | — | MacBook |
| Synthetic 20M, 256 stocks | 20.0M | passed (21.4M level checks, 0 mismatches) | 5.6 M msgs/s | 179 | 2-vCPU x86 cloud VM (dev sanity check only) |

## Layout

```
include/itch/   endian.hpp  messages.hpp  parser.hpp  encoder.hpp  synthetic.hpp  mapped_file.hpp
include/lob/    map_book.hpp  book_builder.hpp  checker.hpp
tools/          itch_stats  itch_slice  gen_synthetic  book_replay
tests/          GoogleTest unit tests
scripts/        fetch_sample.sh
results/        JSON written by the tools; README tables are filled from these
```
