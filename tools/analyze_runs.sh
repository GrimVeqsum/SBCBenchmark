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

pretty_table() {
  if command -v column >/dev/null 2>&1; then
    column -t -s $'\t'
  else
    cat
  fi
}

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
      (.scenario_metrics.cpu_ops_avg // -1),
      (.scenario_metrics.nn_inf_per_sec_avg // -1),
      (.scenario_metrics.storage_mb_s_avg // -1),
      (.scenario_metrics.ping_p99_ms_avg // -1),
      (.scenario_metrics.packet_loss_pct_avg // -1)
    ] | @tsv
  ' "$d/summary.json" >> "$tmp"
done

if [ ! -s "$tmp" ]; then
  echo "No summary.json found under $ROOT"
  exit 0
fi

echo "=== RUNS (sortable master table) ==="
{
  echo -e "run_dir\tscenario\tscore\ttemp_max\tpower_avg\tcpu_ops_avg\tnn_inf_avg\tstorage_avg\tping_p99_avg\tloss_avg"
  sort -t$'\t' -k2,2 -k3,3nr "$tmp"
} | pretty_table

echo
echo "=== BEST BY SCENARIO ==="
echo -e "scenario\tbest_run\tscore\ttemp_max\tpower_avg\tcpu_ops_avg\tnn_inf_avg\tstorage_avg\tping_p99_avg\tloss_avg"
awk -F'\t' '
{
  sc=$2; score=$3+0
  if (!(sc in best) || score > best_score[sc]) {
    best[sc]=$0
    best_score[sc]=score
  }
}
END {
  for (sc in best) {
    split(best[sc], a, "\t")
    print sc "\t" a[1] "\t" a[3] "\t" a[4] "\t" a[5] "\t" a[6] "\t" a[7] "\t" a[8] "\t" a[9] "\t" a[10]
  }
}
' "$tmp" | sort | pretty_table

latest="$(ls -td "$ROOT"/* 2>/dev/null | head -n1 || true)"
if [ -n "$latest" ] && [ -f "$latest/summary.json" ]; then
  echo
  echo "=== LATEST: $latest ==="
  jq -r '
    "scenario: \(.scenario)",
    "description: \(.description // "")",
    "score: \(.score)",
    "primary metrics:",
    (.metric_profile.primary_metrics[] | " - " + .),
    "",
    "scenario metrics:",
    (.scenario_metrics | to_entries[] | " - \(.key)=\(.value)"),
    "",
    "steps:",
    (.steps[] | " - \(.name) [\(.kind_name // .kind)]: cpu_ops=\(.ops_per_sec), nn_inf=\(.nn_inf_per_sec), storage=\(.throughput_mb_s), ping_p99=\(.ping_p99_ms), loss=\(.packet_loss_pct)")
  ' "$latest/summary.json"
fi