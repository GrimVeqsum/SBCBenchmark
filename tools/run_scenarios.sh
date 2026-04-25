#!/usr/bin/env bash
set -euo pipefail

BIN="${1:-./sbc_bench_v4}"
SCALE="${2:-0.05}"

if [ ! -x "$BIN" ]; then
  echo "Binary '$BIN' not found or not executable" >&2
  echo "Build example: gcc -std=c11 -O2 -pthread sbc_bench_v4.c -lm -o sbc_bench_v4" >&2
  exit 1
fi

scenarios=(baseline iot server_gateway embedded neural_host)

echo "Running scenario pack: baseline / iot / server_gateway / embedded / neural_host"
echo "Duration scale: $SCALE"

for sc in "${scenarios[@]}"; do
  echo
  echo ">>> scenario=$sc"
  "$BIN" "$sc" "$SCALE"
done

echo

echo "Done. Analyze results with:"
echo "  ./tools/analyze_runs.sh"