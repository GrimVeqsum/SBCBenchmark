cat > tools/run_scenarios.sh <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

BIN="${1:-./sbc_bench_v4}"
SCALE="${2:-0.05}"

if [ ! -x "$BIN" ]; then
  echo "Binary '$BIN' not found or not executable" >&2
  exit 1
fi

scenarios=(baseline server_edge embedded neural iot_controller)

echo "Running scenarios with scale=$SCALE"
for sc in "${scenarios[@]}"; do
  echo
  echo ">>> $sc"
  "$BIN" "$sc" "$SCALE"
done

echo
echo "Done. Analyze:"
echo "  ./tools/analyze_runs.sh"
EOF

chmod +x tools/run_scenarios.sh