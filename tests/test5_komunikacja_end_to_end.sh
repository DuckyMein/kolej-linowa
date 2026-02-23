#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"
source "./common.sh"

TEST_NAME="test5_komunikacja_end_to_end"

build_project
reset_logs

echo "== $TEST_NAME =="

# Mały, czytelny przebieg demo
run_main_bg 20 25
PID_MAIN="$RUN_MAIN_PID"

# Poczekaj aż pojawi się klient, który wykonał przejazd (ARRIVE) – wtedy mamy pewność end-to-end
client_line=""
for _ in {1..200}; do
  if [[ -f "$OUTPUT_DIR/klienci.log" ]]; then
    client_line="$(grep -m1 -E "KLIENT [0-9]+: ARRIVE\b" "$OUTPUT_DIR/klienci.log" 2>/dev/null || true)"
  fi
  [[ -n "$client_line" ]] && break
  sleep 0.1
done

client_id=""
client_pid=""
if [[ -n "$client_line" ]]; then
  client_pid="$(echo "$client_line" | sed -n "s/.*\[PID \([0-9]\+\)\].*/\1/p")"
  client_id="$(echo "$client_line" | sed -n "s/.*KLIENT \([0-9]\+\):.*/\1/p")"
fi

# Snapshot z monitora w trakcie (jeśli dostępny)
MONITOR_SNAP=""
if [[ -x "$APP_DIR/monitor" ]]; then
  MONITOR_SNAP="$(cd "$APP_DIR" && ./monitor --once --no-color 2>/dev/null || true)"
fi

wait_main "$PID_MAIN"

OUTDIR="$(collect_results "$TEST_NAME")"

{
  echo "# $TEST_NAME"
  echo
  echo "Cel: pokazać łańcuch komunikacji IPC (klient → kasa → bramka → peron → wyciąg) na jednym przykładzie." 
  echo
  echo "Wybrany klient (z logów): id=${client_id:-?} pid=${client_pid:-?}"
  echo
  if [[ -n "$MONITOR_SNAP" ]]; then
    echo "## Snapshot MONITOR (w trakcie działania)"
    echo '```'
    echo "$MONITOR_SNAP"
    echo '```'
    echo
  fi

  if [[ -n "$client_pid" ]]; then
    echo "## Dowody w logach (ten sam pid w wielu procesach)"
    echo
    echo "### klient (output/klienci.log)"
    grep -E "\[PID ${client_pid}\]" "$OUTDIR/klienci.log" | head -n 80 || true
    echo
    echo "### kasa (output/kasa.log)"
    grep -E "(\[PID ${client_pid}\]|pid=${client_pid})" "$OUTDIR/kasa.log" | head -n 80 || true
    echo
    echo "### bramki (output/bramki.log)"
    grep -E "(\[PID ${client_pid}\]|pid=${client_pid})" "$OUTDIR/bramki.log" | head -n 80 || true
    echo
    echo "### main (output/main.log)"
    grep -E "Kasjer uruchomiony|Bramka [0-9]+ uruchomiona|Wyci[aą]g uruchomiony|Generator uruchomiony|Wszystkie procesy" "$OUTDIR/main.log" | head -n 40 || true
  else
    echo "[WARN] Nie udało się wybrać klienta z logu (możliwe, że w tym uruchomieniu nie wygenerowano klientów)."
    echo "Sprawdź: $OUTDIR/klienci.log (czy są linie ARRIVE)"
  fi

  echo
  echo "## Interpretacja (do prezentacji)"
  echo "- Ten sam pid widoczny w logach kasy i bramek oznacza, że komunikaty MQ zostały odebrane i obsłużone." 
  echo "- Klient loguje przejścia przez etapy (kasa → bramka → peron → BOARD/ARRIVE), co domyka ścieżkę end-to-end." 
} > "$OUTDIR/summary.txt"

fail=0
if [[ -z "$client_pid" ]]; then
  echo "[FAIL] Nie znaleziono klienta z ARRIVE (brak dowodu end-to-end)" >&2
  fail=1
else
  in_kasa="$(grep -c -E "(\[PID ${client_pid}\]|pid=${client_pid})" "$OUTDIR/kasa.log" 2>/dev/null || true)"
  in_bramki="$(grep -c -E "(\[PID ${client_pid}\]|pid=${client_pid})" "$OUTDIR/bramki.log" 2>/dev/null || true)"
  if [[ "$in_kasa" -le 0 ]]; then
    echo "[FAIL] PID klienta ($client_pid) nie pojawił się w kasa.log" >&2
    fail=1
  fi
  if [[ "$in_bramki" -le 0 ]]; then
    echo "[FAIL] PID klienta ($client_pid) nie pojawił się w bramki.log" >&2
    fail=1
  fi
fi

print_hint_screenshots "$OUTDIR"
exit "$fail"
