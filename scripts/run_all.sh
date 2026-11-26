#!/usr/bin/env bash
# Runs every benchmark and check on one ITCH file, then builds results/REPORT.md.
#
#   scripts/run_all.sh [ITCH_FILE] [PIPELINE_RATE_M]
#
# Defaults: data/12302019.slice.itch and a 4M msgs/s paced pipeline run.
# Takes roughly 10-15 minutes on a laptop. Close other apps first: background
# load shows up as noise in the medians and as spikes in the latency tails.
set -euo pipefail
cd "$(dirname "$0")/.."

FILE="${1:-data/12302019.slice.itch}"
RATE="${2:-4}"
[ -f "$FILE" ] || { echo "missing $FILE (run scripts/fetch_sample.sh first)"; exit 1; }

cmake --build build -j
mkdir -p results

step() { echo; echo "=================== $* ==================="; }

step "1/7 unit tests";                       ctest --test-dir build --output-on-failure
step "2/7 parser throughput";                ./build/itch_stats "$FILE" --reps 5
step "3/7 book correctness (map + fast)";    ./build/book_replay "$FILE" --check
step "4/7 book ablation";                    ./build/ablation "$FILE" --reps 5
step "5/7 book latency per message";         ./build/book_replay "$FILE" --latency
step "6/7 matching engine";                  ./build/engine_bench --reps 5
step "7/7 pipeline";                         ./build/pipeline_bench "$FILE" --reps 5 --rate "$RATE"

echo
python3 scripts/report.py
echo "done: open results/REPORT.md"
