#!/usr/bin/env bash
set -euo pipefail
# Test 4 – Koniec dnia (CLOSING/DRAINING/SHUTDOWN) (v2, wait-fix)
# Uwaga: tutaj czas T powinien być na tyle krótki, żeby dotknąć Tk wg parametrów programu,
# ale na tyle długi, żeby w DRAINING były osoby na terenie/peronie.

source "$(dirname "$0")/common.sh"

reset_logs
build_project

N="${1:-180}"
T="${2:-40}"

run_main_bg "$N" "$T"
PID="$RUN_MAIN_PID"
wait_main "$PID" || true

OUTDIR="$(collect_results "test4_koniec_dnia_drenowanie")"

MAIN_LOG="$OUTPUT_DIR/main.log"
WYCIAG_LOG="$OUTPUT_DIR/wyciag.log"

{
  echo "TEST4: koniec dnia CLOSING/DRAINING/SHUTDOWN"
  echo "N=$N, CZAS=$T"
  echo
  echo "[MAIN] Fazy dnia (fragment):"
  grep -E "FAZA|CLOSING|DRAINING|SHUTDOWN|czas_konca_dnia" "$MAIN_LOG" | head -n 120 || true
  echo
  echo "[WYCIAG] Drenowanie + 3 sekundy (fragment):"
  grep -E "DRAIN|dren|wyłączam|3" "$WYCIAG_LOG" | tail -n 120 || true
} > "$OUTDIR/summary.txt"

print_hint_screenshots "$OUTDIR"
