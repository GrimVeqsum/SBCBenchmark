mkdir -p tools
cat > tools/analyze_runs.sh <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

ROOT="${1:-runs_v4_c}"

if [ ! -d "$ROOT" ]; then
  echo "Directory '$ROOT' not found" >&2
  exit 1
fi

if ! command -v jq >/dev/null 2>&1; then
  echo "jq is required: sudo apt-get install -y jq" >&2
  exit 1
fi

tmp="$(mktemp)"
trap 'rm -f "$tmp"' EXIT

for d in "$ROOT"/*; do
  [ -f "$d/summary.json" ] || continue
  jq -r --arg run "$(basename "$d")" '
    [
      $run,
      (.scenario // "n/a"),
      (.score // -1),
      (.aggregates.temp_c_max // -1),
      (.aggregates.power_w_avg // -1),
      (.aggregates.cpu_freq_mhz_avg // -1),
      (.aggregates.cpu_util_pct_avg // -1)
    ] | @tsv
  ' "$d/summary.json" >> "$tmp"
done

if [ ! -s "$tmp" ]; then
  echo "No summary.json found under $ROOT"
  exit 0
fi

echo "=== RUNS ==="
{
  echo -e "run_dir\tscenario\tscore\ttemp_max\tpower_avg\tcpu_freq_avg\tcpu_util_avg"
  sort -t$'\t' -k2,2 -k3,3nr "$tmp"
} | column -t -s $'\t'

echo
echo "=== BEST BY SCENARIO ==="
awk -F'\t' '
{
  sc=$2; score=$3+0
  if (!(sc in best) || score > best_score[sc]) {
    best[sc]=$0
    best_score[sc]=score
  }
}
END {
  print "scenario\tbest_run\tscore\ttemp_max\tpower_avg\tcpu_freq_avg\tcpu_util_avg"
  for (sc in best) {
    split(best[sc], a, "\t")
    print sc "\t" a[1] "\t" a[3] "\t" a[4] "\t" a[5] "\t" a[6] "\t" a[7]
  }
}
' "$tmp" | sort | column -t -s $'\t'

latest="$(ls -td "$ROOT"/* 2>/dev/null | head -n1 || true)"
if [ -n "$latest" ] && [ -f "$latest/summary.json" ]; then
  echo
  echo "=== LATEST: $latest ==="
  jq -r '
    "scenario: \(.scenario)",
    "score: \(.score)",
    "temp_max: \(.aggregates.temp_c_max)",
    "power_avg: \(.aggregates.power_w_avg)",
    "",
    "steps:",
    (.steps[] | " - \(.name) [\(.kind_name // .kind)]: cpu_ops=\(.ops_per_sec), nn_inf=\(.nn_inf_per_sec), storage=\(.throughput_mb_s), ping_p99=\(.ping_p99_ms), loss=\(.packet_loss_pct)")
  ' "$latest/summary.json"
fi
EOF

chmod +x tools/analyze_runs.sh