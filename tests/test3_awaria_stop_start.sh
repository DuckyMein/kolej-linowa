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

# STOP/START wysyłamy do main – main przekazuje do pracownika (wymaganie z projektu)
mainpid="$PID"

# STOP (SIGUSR1)
kill -USR1 "$mainpid" || true
sleep 2

# START (SIGUSR2)
kill -USR2 "$mainpid" || true

# poczekaj na koniec symulacji
wait_main "$PID" || true

OUTDIR="$(collect_results "test3_awaria_stop_start")"

PRAC_LOG="$OUTPUT_DIR/pracownicy.log"
MAIN_LOG="$OUTPUT_DIR/main.log"

# Asercje na konkretnych wzorcach z logów
stop_initiated="$(grep -c -E "PRACOWNIK[12]: STOP \(inicjator\)" "$PRAC_LOG" 2>/dev/null || true)"
gotowy_stop="$(grep -c -E "PRACOWNIK[12]: (Drugi pracownik GOTOWY \(STOP\)|Otrzymano STOP.*potwierdzam GOTOWY|P[12] GOTOWY)" "$PRAC_LOG" 2>/dev/null || true)"
main_awaria="$(grep -c -E "SYGNAŁ SIGUSR1.*AWARIA" "$MAIN_LOG" 2>/dev/null || true)"
main_wznow="$(grep -c -E "SYGNAŁ SIGUSR2.*WZNOWIENIE" "$MAIN_LOG" 2>/dev/null || true)"

{
  echo "TEST3: awaria STOP/START"
  echo "N=$N, CZAS=$T"
  echo
  echo "[MAIN] Sygnały odebrane:"
  grep -E "SYGNAŁ SIGUSR" "$MAIN_LOG" | head -n 20 || true
  echo
  echo "[PRACOWNICY] Procedura STOP/GOTOWY/START:"
  grep -E "PRACOWNIK[12]:.*(STOP|GOTOWY|START|Otrzymano|wznawiam|zatrzymana)" "$PRAC_LOG" | head -n 80 || true
  echo
  echo "## Asercje"
  echo "- main: AWARIA(SIGUSR1)=$main_awaria WZNOWIENIE(SIGUSR2)=$main_wznow"
  echo "- pracownicy: STOP_initiated=$stop_initiated GOTOWY=$gotowy_stop"
} > "$OUTDIR/summary.txt"

fail=0
if [[ "$main_awaria" -le 0 ]]; then
  echo "[FAIL] main nie odebrał SIGUSR1 (AWARIA/STOP)" >&2
  fail=1
fi
if [[ "$main_wznow" -le 0 ]]; then
  echo "[FAIL] main nie odebrał SIGUSR2 (WZNOWIENIE/START)" >&2
  fail=1
fi
if [[ "$stop_initiated" -le 0 ]]; then
  echo "[FAIL] Żaden pracownik nie zainicjował procedury STOP" >&2
  fail=1
fi
if [[ "$gotowy_stop" -le 0 ]]; then
  echo "[FAIL] Brak potwierdzenia GOTOWY między pracownikami" >&2
  fail=1
fi

print_hint_screenshots "$OUTDIR"
exit "$fail"