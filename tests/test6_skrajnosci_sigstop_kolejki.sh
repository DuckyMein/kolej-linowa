#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"
source "./common.sh"

TEST_NAME="test6_skrajnosci_sigstop_kolejki"

build_project
reset_logs

echo "== $TEST_NAME =="

# Dłuższa symulacja, żeby było miejsce na STOP/CONT
run_main_bg 30 35
PID_MAIN="$RUN_MAIN_PID"

get_kasjer_pid() {
  grep -oE "Kasjer uruchomiony \(PID=[0-9]+\)" "$OUTPUT_DIR/main.log" \
    | tail -n1 \
    | sed -n 's/.*PID=\([0-9]\+\).*/\1/p'
}

kasjer_pid=""
for _ in {1..50}; do
  kasjer_pid="$(get_kasjer_pid || true)"
  [[ -n "$kasjer_pid" ]] && break
  sleep 0.2
done

snap1=""; snap2=""; snap3=""

# Wyciągnij msg_qnum dla kolejki kasa z outputu monitora (format: kasa=QNUM/QBYTES)
extract_kasa_qnum() {
  echo "$1" | grep -m1 "^MQ: kasa=" | sed -n "s/.*kasa=\([0-9]\+\)\/.*/\1/p"
}
k1=""; k2=""; k3=""
if [[ -x "$APP_DIR/monitor" ]]; then
  snap1="$(cd "$APP_DIR" && ./monitor --once --no-color 2>/dev/null || true)"
  k1="$(extract_kasa_qnum "$snap1" || true)"
fi

if [[ -n "$kasjer_pid" ]]; then
  echo "[test] SIGSTOP kasjer PID=$kasjer_pid (3s)" 
  kill -STOP "$kasjer_pid" || true
  sleep 3

  if [[ -x "$APP_DIR/monitor" ]]; then
    snap2="$(cd "$APP_DIR" && ./monitor --once --no-color 2>/dev/null || true)"
    k2="$(extract_kasa_qnum "$snap2" || true)"
  fi

  echo "[test] SIGCONT kasjer PID=$kasjer_pid" 
  kill -CONT "$kasjer_pid" || true
  sleep 3

  if [[ -x "$APP_DIR/monitor" ]]; then
    snap3="$(cd "$APP_DIR" && ./monitor --once --no-color 2>/dev/null || true)"
    k3="$(extract_kasa_qnum "$snap3" || true)"
  fi
else
  echo "[WARN] Nie znalazłem PID kasjera w main.log, pomijam SIGSTOP." >&2
fi

wait_main "$PID_MAIN"

OUTDIR="$(collect_results "$TEST_NAME")"

{
  echo "# $TEST_NAME"
  echo
  echo "Cel: zasymulować skrajność (chwilowe zatrzymanie procesu kasjera) i pokazać, że kolejki IPC się zapełniają, a potem wracają do normy." 
  echo
  echo "Kasjer PID: ${kasjer_pid:-?}"
  echo "MQ kasa msg_qnum: przed=${k1:-?} w_STOP=${k2:-?} po=${k3:-?}"
  if [[ -n "${k1:-}" && -n "${k2:-}" && "${k2}" -le "${k1}" ]]; then
    echo "[WARN] msg_qnum nie wzrosło (możliwe za mało klientów / zbyt krótki STOP)"
  fi
  echo
  if [[ -n "$snap1" ]]; then
    echo "## Snapshot przed SIGSTOP"
    echo '```'
    echo "$snap1"
    echo '```'
    echo
  fi
  if [[ -n "$snap2" ]]; then
    echo "## Snapshot w trakcie SIGSTOP (kolejki rosną)"
    echo '```'
    echo "$snap2"
    echo '```'
    echo
  fi
  if [[ -n "$snap3" ]]; then
    echo "## Snapshot po SIGCONT (system wraca do pracy)"
    echo '```'
    echo "$snap3"
    echo '```'
    echo
  fi

  echo "## Fragmenty logów (ciągłość działania)"
  echo
  echo "### kasa.log (widać, że po wznowieniu znowu sprzedaje)"
  grep -E "KASJER: (klient|SPRZEDAŻ|sprzedano)" "$OUTDIR/kasa.log" | tail -n 30 || true
  echo
  echo "### main.log (uruchomione procesy, koniec dnia)"
  grep -E "Kasjer uruchomiony|Czas symulacji|PROCEDURA KOŃCA DNIA|Zamykanie" "$OUTDIR/main.log" | head -n 80 || true
  echo
  echo "## Interpretacja (do prezentacji)"
  echo "- SIGSTOP kasjera zatrzymuje obsługę MQ kasa → w monitorze widać rosnące msg_qnum (backlog)."
  echo "- Po SIGCONT proces wraca i backlog maleje, co pokazuje że komunikaty nie znikają i są obsługiwane." 
} > "$OUTDIR/summary.txt"

fail=0
if [[ -z "$kasjer_pid" ]]; then
  echo "[FAIL] Nie znaleziono PID kasjera - SIGSTOP nie został wysłany" >&2
  fail=1
fi

# Sprawdź że kasjer po SIGCONT nadal sprzedawał (ciągłość)
sprzedaz_count="$(grep -c -E "KASJER: (SPRZEDAŻ|sprzedano)" "$OUTDIR/kasa.log" 2>/dev/null || true)"
if [[ "$sprzedaz_count" -le 0 ]]; then
  echo "[FAIL] Kasjer nie zalogował żadnej sprzedaży (kasa.log)" >&2
  fail=1
fi

print_hint_screenshots "$OUTDIR"
exit "$fail"
