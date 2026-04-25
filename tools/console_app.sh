#!/usr/bin/env bash
set -euo pipefail

BIN_DEFAULT="./sbc_bench_v4"
RUNS_ROOT_DEFAULT="runs_v4_c"

if ! command -v gcc >/dev/null 2>&1; then
  echo "gcc не найден. Установите компилятор C." >&2
  exit 1
fi

clear_screen() {
  printf '\033[2J\033[H'
}

pause() {
  echo
  read -r -p "Нажмите Enter для продолжения..." _
}

print_header() {
  clear_screen
  cat <<'HDR'
============================================================
 SBC Benchmark Console (v4)
============================================================
 Сценарии:
   1) baseline        — базовый профиль
   2) iot             — IoT контроллер
   3) server_gateway  — сервер / шлюз
   4) embedded        — встраиваемое устройство
   5) neural_host     — нейросеть на устройстве
------------------------------------------------------------
HDR
}

ensure_binary() {
  local bin="$1"
  if [ ! -x "$bin" ]; then
    echo "Бинарник '$bin' не найден. Выполняю сборку..."
    gcc -std=c11 -O2 -pthread sbc_bench_v4.c -lm -o "$bin"
    echo "Сборка завершена: $bin"
  fi
}

run_single() {
  local bin="$1"
  echo "Введите имя сценария (baseline/iot/server_gateway/embedded/neural_host):"
  read -r scenario
  echo "Введите масштаб длительности (например 0.05, Enter = 1.0):"
  read -r scale
  scale="${scale:-1.0}"

  ensure_binary "$bin"
  echo
  echo "Запуск: $bin $scenario $scale"
  "$bin" "$scenario" "$scale"
}

run_pack() {
  local bin="$1"
  echo "Введите масштаб длительности для всего набора (например 0.05):"
  read -r scale
  scale="${scale:-0.05}"

  ensure_binary "$bin"
  ./tools/run_scenarios.sh "$bin" "$scale"
}

analyze_runs() {
  local root="$1"
  echo "Каталог запусков (Enter = $root):"
  read -r custom
  root="${custom:-$root}"
  ./tools/analyze_runs.sh "$root"
}

show_latest_json() {
  local root="$1"
  if [ ! -d "$root" ]; then
    echo "Каталог '$root' не найден"
    return 0
  fi

  local latest
  latest="$(ls -td "$root"/* 2>/dev/null | head -n1 || true)"
  if [ -z "$latest" ] || [ ! -f "$latest/summary.json" ]; then
    echo "Нет summary.json в $root"
    return 0
  fi

  if command -v jq >/dev/null 2>&1; then
    jq . "$latest/summary.json"
  else
    cat "$latest/summary.json"
  fi
}

main_menu() {
  local bin="$BIN_DEFAULT"
  local runs_root="$RUNS_ROOT_DEFAULT"

  while true; do
    print_header
    cat <<MENU
Выберите действие:
  [1] Показать каталог сценариев (из бинарника)
  [2] Запустить один сценарий
  [3] Запустить полный набор сценариев
  [4] Анализ запусков
  [5] Показать latest summary.json
  [6] Пересобрать бинарник
  [q] Выход
MENU
    echo
    read -r -p "Ваш выбор: " choice

    case "$choice" in
      1)
        ensure_binary "$bin"
        "$bin" --list-scenarios
        pause
        ;;
      2)
        run_single "$bin"
        pause
        ;;
      3)
        run_pack "$bin"
        pause
        ;;
      4)
        analyze_runs "$runs_root"
        pause
        ;;
      5)
        show_latest_json "$runs_root"
        pause
        ;;
      6)
        gcc -std=c11 -O2 -pthread sbc_bench_v4.c -lm -o "$bin"
        echo "Сборка завершена: $bin"
        pause
        ;;
      q|Q)
        echo "Выход."
        break
        ;;
      *)
        echo "Неизвестный выбор: $choice"
        pause
        ;;
    esac
  done
}

main_menu