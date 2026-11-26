#!/usr/bin/env bash
# Streams a NASDAQ ITCH 5.0 sample day and keeps a ~1.5 GB slice cut on a
# frame boundary. Never stores the full ~10 GB uncompressed file.
#
#   scripts/fetch_sample.sh [DATE] [GB]     e.g. scripts/fetch_sample.sh 12302019 1.5
set -euo pipefail

DATE="${1:-12302019}"
GB="${2:-1.5}"
URL="https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/${DATE}.NASDAQ_ITCH50.gz"
OUT="data/${DATE}.slice.itch"
RAW="data/${DATE}.raw.tmp"

mkdir -p data
[ -x build/itch_slice ] || { echo "build first: cmake -S . -B build && cmake --build build -j"; exit 1; }

# Grab a little more than needed, then trim to the last whole message.
BYTES=$(python3 -c "print(int(${GB} * 1e9) + 1_000_000)")
echo "streaming ${URL} -> first ${BYTES} bytes"
# head closes the pipe early on purpose; ignore the resulting SIGPIPE from curl/gunzip.
set +o pipefail
curl -fsSL "${URL}" | gunzip -c | head -c "${BYTES}" > "${RAW}"
set -o pipefail

./build/itch_slice "${RAW}" "${OUT}" --gb "${GB}"
rm -f "${RAW}"
echo "done: ${OUT}"
