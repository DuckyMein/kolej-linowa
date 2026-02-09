#!/usr/bin/env bash
set -euo pipefail
# Test 3 – Awaria STOP/START (v2, wait-fix)

source "$(dirname "$0")/common.sh"

reset_logs
build_project

N="${1:-120}"
T="${2:-35}"

# uruchom w tle
run_main_bg "$N" "$T"
PID="$RUN_MAIN_PID"

# daj czas na start procesów
sleep 2

# znajdź pracowników
p1="$(pgrep -n pracownik1 || true)"
p2="$(pgrep -n pracownik2 || true)"

# STOP z P1 (SIGUSR1)
if [[ -n "$p1" ]]; then
  kill -USR1 "$p1" || true
fi

sleep 2

# START tylko inicjator (SIGUSR2 wysyłamy do main, a main przekaże)
mainpid="$(pgrep -n main || true)"
if [[ -n "$mainpid" ]]; then
  kill -USR2 "$mainpid" || true
fi

# poczekaj na koniec symulacji
wait_main "$PID" || true

OUTDIR="$(collect_results "test3_awaria_stop_start")"

PRAC_LOG="$OUTPUT_DIR/pracownicy.log"
WYCIAG_LOG="$OUTPUT_DIR/wyciag.log"

{
  echo "TEST3: awaria STOP/START"
  echo "N=$N, CZAS=$T"
  echo
  echo "[PRACOWNICY] Szukamy STOP/START/GOTOWY:"
  grep -E "STOP|START|GOTOWY" "$PRAC_LOG" | head -n 80 || true
  echo
  echo "[WYCIAG] Potwierdzenie zatrzymania/wznowienia (jeśli loguje):"
  grep -E "AWARIA|WZNOW|barier|wznow" "$WYCIAG_LOG" | head -n 80 || true
} > "$OUTDIR/summary.txt"

print_hint_screenshots "$OUTDIR"
