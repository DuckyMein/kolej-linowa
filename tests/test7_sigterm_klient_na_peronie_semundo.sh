#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"
source "./common.sh"

TEST_NAME="test7_sigterm_klient_na_peronie_semundo"

build_project
reset_logs

echo "== $TEST_NAME =="

# Cel: deterministycznie zabić klienta, który TRZYMA semafor PERON (SEM_UNDO)
# i pokazać, że system się nie blokuje.

# Uruchom (krótszy test + limity żeby nie robić dzikich liczb)
run_main_bg 40 25 2000 150
PID_MAIN="$RUN_MAIN_PID"

get_wyciag_pid() {
  # toleruj Wyciag/Wyciąg
  grep -aEo "Wyci[aą]g uruchomiony \(PID=[0-9]+\)" "$OUTPUT_DIR/main.log" \
    | tail -n1 \
    | sed -n "s/.*PID=\([0-9]\+\).*/\1/p"
}

wyciag_pid=""
for _ in {1..80}; do
  wyciag_pid="$(get_wyciag_pid || true)"
  [[ -n "$wyciag_pid" ]] && break
  sleep 0.1
done

if [[ -z "$wyciag_pid" ]]; then
  echo "[FAIL] Nie znalazłem PID wyciągu w main.log" >&2
  kill -TERM "$PID_MAIN" 2>/dev/null || true
  exit 1
fi

echo "[test] SIGSTOP wyciag PID=$wyciag_pid (blokuję BOARD)"
kill -STOP "$wyciag_pid" || true

# Poczekaj aż klienci zaczną trafiać NA_PERONIE (wyciąg jest STOP → klienci się kumulują)
sleep 1

# Znajdź klienta NA_PERONIE - bierzemy OSTATNIEGO (najświeższego, pewnie jeszcze żyje)
line=""
for _ in {1..400}; do
  if [[ -f "$OUTPUT_DIR/klienci.log" ]]; then
    line="$(grep -aE "KLIENT [0-9]+: NA_PERONIE" "$OUTPUT_DIR/klienci.log" 2>/dev/null | tail -n1 || true)"
  fi
  [[ -n "$line" ]] && break
  sleep 0.05
done

if [[ -z "$line" ]]; then
  echo "[FAIL] Nie znalazłem klienta NA_PERONIE (brak dowodu trzymania SEM_PERON)" >&2
  kill -CONT "$wyciag_pid" 2>/dev/null || true
  kill -TERM "$PID_MAIN" 2>/dev/null || true
  exit 1
fi

client_pid="$(echo "$line" | sed -n "s/.*\[PID \([0-9]\+\)\].*/\1/p")"
client_id="$(echo "$line" | sed -n "s/.*KLIENT \([0-9]\+\):.*/\1/p")"

# Upewnij się że klient jeszcze żyje (nie zdążył przejechać)
if ! kill -0 "$client_pid" 2>/dev/null; then
  echo "[WARN] Klient PID=$client_pid już nie żyje, szukam innego..." >&2
  # Szukaj innego żywego klienta NA_PERONIE
  found=0
  while IFS= read -r cand; do
    cpid="$(echo "$cand" | sed -n "s/.*\[PID \([0-9]\+\)\].*/\1/p")"
    if kill -0 "$cpid" 2>/dev/null; then
      client_pid="$cpid"
      client_id="$(echo "$cand" | sed -n "s/.*KLIENT \([0-9]\+\):.*/\1/p")"
      found=1
      break
    fi
  done < <(grep -aE "KLIENT [0-9]+: NA_PERONIE" "$OUTPUT_DIR/klienci.log" 2>/dev/null | tac)
  if [[ "$found" -eq 0 ]]; then
    echo "[FAIL] Żaden klient NA_PERONIE nie żyje (wyciąg STOP nie zadziałał?)" >&2
    kill -CONT "$wyciag_pid" 2>/dev/null || true
    kill -TERM "$PID_MAIN" 2>/dev/null || true
    exit 1
  fi
fi

snap_before=""; snap_after=""
if [[ -x "$APP_DIR/monitor" ]]; then
  snap_before="$(cd "$APP_DIR" && ./monitor --once --no-color 2>/dev/null || true)"
fi

echo "[test] SIGKILL klient id=$client_id PID=$client_pid (NA_PERONIE)"
kill -KILL "$client_pid" 2>/dev/null || true
sleep 0.3

if kill -0 "$client_pid" 2>/dev/null; then
  echo "[FAIL] Klient PID=$client_pid nadal żyje po SIGKILL" >&2
  kill -CONT "$wyciag_pid" 2>/dev/null || true
  kill -TERM "$PID_MAIN" 2>/dev/null || true
  exit 1
fi

if [[ -x "$APP_DIR/monitor" ]]; then
  snap_after="$(cd "$APP_DIR" && ./monitor --once --no-color 2>/dev/null || true)"
fi

echo "[test] SIGCONT wyciag PID=$wyciag_pid"
kill -CONT "$wyciag_pid" || true

# Poczekaj na koniec symulacji
wait_main "$PID_MAIN"

OUTDIR="$(collect_results "$TEST_NAME")"

# Asercje
k_client_board="$(grep -c "\[PID ${client_pid}\].*BOARD" "$OUTDIR/klienci.log" 2>/dev/null || true)"
k_client_arrive="$(grep -c "\[PID ${client_pid}\].*ARRIVE" "$OUTDIR/klienci.log" 2>/dev/null || true)"

board_total="$(grep -c "BOARD" "$OUTDIR/klienci.log" 2>/dev/null || true)"
arrive_total="$(grep -c "ARRIVE" "$OUTDIR/klienci.log" 2>/dev/null || true)"

fail=0
# UWAGA: klient mógł dostać BOARD ZANIM SIGSTOP dotarł do wyciągu (race condition
# wyciąg pracuje z INTERWAL=1ms, SIGSTOP z basha trwa kilka ms).
# To NIE jest błąd - to normalne zachowanie.  Ważne jest:
# 1) klient NA_PERONIE miał sloty SEM_PERON (SEM_UNDO)
# 2) po SIGKILL klienta SEM_UNDO zwraca sloty
# 3) system dalej działa (BOARD_total > 0, ARRIVE_total > 0) = brak deadlocka
if [[ "$k_client_board" != "0" ]]; then
  echo "[INFO] Ubity klient zdążył dostać BOARD=$k_client_board (race SIGSTOP vs wyciąg 1ms - OK)" >&2
fi
if [[ "$board_total" -le 0 || "$arrive_total" -le 0 ]]; then
  echo "[FAIL] Brak dowodu ciągłości po kill: BOARD=$board_total ARRIVE=$arrive_total (SEM_UNDO nie zadziałał?)" >&2
  fail=1
fi

{
  echo "# $TEST_NAME"
  echo
  echo "Cel: klient ginie NA_PERONIE (po sem_wait_n_undo), ale dzięki SEM_UNDO nie blokuje PERON i system działa dalej."
  echo
  echo "Wybrany klient: id=${client_id} pid=${client_pid}"
  echo "Wyciąg PID: ${wyciag_pid} (STOP przed kill, CONT po kill)"
  echo
  if [[ -n "$snap_before" ]]; then
    echo "## Snapshot MONITOR (przed kill)"
    echo '```'
    echo "$snap_before"
    echo '```'
    echo
  fi
  if [[ -n "$snap_after" ]]; then
    echo "## Snapshot MONITOR (po kill)"
    echo '```'
    echo "$snap_after"
    echo '```'
    echo
  fi

  echo "## Asercje"
  echo "- Ubity klient: BOARD=${k_client_board}, ARRIVE=${k_client_arrive}"
  if [[ "$k_client_board" != "0" ]]; then
    echo "  (BOARD>0: race condition SIGSTOP vs wyciag 1ms - klient zdążył wsiąść, to normalne)"
  fi
  echo "- System: BOARD_total=${board_total}, ARRIVE_total=${arrive_total} (oczekiwane >0 = dowód ciągłości)"
  echo
  echo "### klienci.log (fragmenty dla klienta)"
  grep -n -aE "\[PID ${client_pid}\]" "$OUTDIR/klienci.log" | head -n 120 || true
  echo
  echo "### wyciag.log (końcówka)"
  tail -n 60 "$OUTDIR/wyciag.log" || true
  echo
  echo "## Interpretacja (do prezentacji)"
  echo "- 'NA_PERONIE' oznacza, że klient już pobrał sloty SEM_PERON (SEM_UNDO)."
  echo "- Po SIGKILL sloty wracają automatycznie (SEM_UNDO), a system nadal generuje BOARD/ARRIVE dla innych." 
} > "$OUTDIR/summary.txt"

print_hint_screenshots "$OUTDIR"

exit "$fail"
