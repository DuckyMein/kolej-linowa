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

# Asercje: sprawdź czy każda faza wystąpiła
faza1="$(grep -c "FAZA 1: CLOSING" "$MAIN_LOG" 2>/dev/null || true)"
faza2="$(grep -c "FAZA 2: DRAINING" "$MAIN_LOG" 2>/dev/null || true)"
faza3="$(grep -c "FAZA 3: SHUTDOWN" "$MAIN_LOG" 2>/dev/null || true)"
drain_wyciag="$(grep -c -E "Drenowanie zakończone.*wyłączam za 3s" "$WYCIAG_LOG" 2>/dev/null || true)"

{
  echo "TEST4: koniec dnia CLOSING/DRAINING/SHUTDOWN"
  echo "N=$N, CZAS=$T"
  echo
  echo "[MAIN] Fazy dnia (fragment):"
  grep -E "FAZA [123]:|faza_dnia" "$MAIN_LOG" | head -n 120 || true
  echo
  echo "[WYCIAG] Drenowanie (fragment):"
  grep -E "Drenowanie|wyłączam" "$WYCIAG_LOG" | tail -n 30 || true
  echo
  echo "## Asercje"
  echo "- FAZA1_CLOSING=$faza1 FAZA2_DRAINING=$faza2 FAZA3_SHUTDOWN=$faza3"
  echo "- Wyciąg drenowanie+3s=$drain_wyciag"
} > "$OUTDIR/summary.txt"

fail=0
if [[ "$faza1" -le 0 ]]; then
  echo "[FAIL] Brak FAZA 1: CLOSING w main.log" >&2
  fail=1
fi
if [[ "$faza2" -le 0 ]]; then
  echo "[FAIL] Brak FAZA 2: DRAINING w main.log" >&2
  fail=1
fi
if [[ "$faza3" -le 0 ]]; then
  echo "[FAIL] Brak FAZA 3: SHUTDOWN w main.log" >&2
  fail=1
fi
if [[ "$drain_wyciag" -le 0 ]]; then
  echo "[FAIL] Wyciąg nie zalogował drenowania (Drenowanie zakończone...wyłączam za 3s)" >&2
  fail=1
fi

print_hint_screenshots "$OUTDIR"
exit "$fail"
