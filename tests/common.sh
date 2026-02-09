#!/usr/bin/env bash
set -euo pipefail

# common.sh (v2)
# - działa w WSL/SSH/tmux
# - NIE używa command substitution do uruchamiania main w tle (bo wtedy PID nie jest "child" -> wait fail)
# - wykrywa katalog aplikacji (tam gdzie jest ./main) i katalog logów output/

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Wykryj gdzie jest ./main (w projekcie zwykle ROOT_DIR albo ROOT_DIR/kolej)
detect_app_dir() {
  if [[ -x "$ROOT_DIR/main" ]]; then
    echo "$ROOT_DIR"
    return 0
  fi
  if [[ -x "$ROOT_DIR/kolej/main" ]]; then
    echo "$ROOT_DIR/kolej"
    return 0
  fi
  # fallback: szukaj w głębokości 2
  local found
  found="$(find "$ROOT_DIR" -maxdepth 2 -type f -name main -perm -u+x 2>/dev/null | head -n1 || true)"
  if [[ -n "$found" ]]; then
    echo "$(cd "$(dirname "$found")" && pwd)"
    return 0
  fi
  echo "[common.sh] Nie znaleziono ./main (uruchom make?)" >&2
  exit 1
}

APP_DIR="$(detect_app_dir)"
OUTPUT_DIR="$APP_DIR/output"
TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RESULTS_DIR="$TESTS_DIR/results"

require_cmd() { command -v "$1" >/dev/null 2>&1 || { echo "[common.sh] Brak polecenia: $1" >&2; exit 1; }; }

build_project() {
  require_cmd make
  (cd "$APP_DIR" && make -s)
}

reset_logs() {
  mkdir -p "$OUTPUT_DIR"
  local f
  for f in main pracownicy generator kasa bramki wyciag klienci sprzatacz; do
    : > "$OUTPUT_DIR/${f}.log" || true
  done
}

# Uruchom ./main w tle i zapisz PID do globalnej zmiennej RUN_MAIN_PID.
# Nie wolno wywoływać tego w $(...) bo wtedy bash robi subshell i wait nie zadziała.
RUN_MAIN_PID=""
run_main_bg() {
  local N="$1"; local T="$2"
  mkdir -p "$OUTPUT_DIR"
  # uruchamiamy bez subshella; pushd/popd zachowuje poprawny PID dla wait
  pushd "$APP_DIR" >/dev/null
  ./main "$N" "$T" > "$OUTPUT_DIR/main.log" 2>&1 &
  RUN_MAIN_PID="$!"
  popd >/dev/null
}

wait_main() {
  local pid="$1"
  # normalnie wait działa, ale jeśli ktoś odpali w dziwny sposób, mamy fallback na poll
  if ! wait "$pid"; then
    true
  fi
}

collect_results() {
  local test_name="$1"
  local ts outdir
  ts="$(date +%Y%m%d_%H%M%S)"
  outdir="$RESULTS_DIR/${test_name}_${ts}"
  mkdir -p "$outdir"
  if ls "$OUTPUT_DIR"/*.log >/dev/null 2>&1; then
    cp -a "$OUTPUT_DIR"/*.log "$outdir/" || true
  fi
  # dodatkowe raporty, jeśli istnieją
  for f in raport.txt log_przejsc.csv log_przejsc.txt; do
    [[ -f "$OUTPUT_DIR/$f" ]] && cp -a "$OUTPUT_DIR/$f" "$outdir/" || true
    [[ -f "$APP_DIR/$f" ]] && cp -a "$APP_DIR/$f" "$outdir/" || true
  done
  echo "$outdir"
}

print_hint_screenshots() {
  local outdir="$1"
  cat <<MSG

[INFO] Wyniki zapisane w: $outdir
[INFO] Do sprawozania:
  - $outdir/summary.txt
  - (opcjonalnie) wybrane logi w $outdir/*.log
MSG
}
