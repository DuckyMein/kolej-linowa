#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"
source "./common.sh"

TEST_NAME="test9_wkrzesle_range_i_drain_zero"

build_project
reset_logs

echo "== $TEST_NAME =="

if [[ ! -x "$APP_DIR/monitor" ]]; then
  echo "[FAIL] Brak ./monitor – test wymaga monitor --once --no-color" >&2
  exit 1
fi

# Ustalenie twardego limitu: (LICZBA_RZEDOW/2) * KRZESLA_W_RZEDZIE
# Dla aktualnego config.h to 18/2 * 4 = 36
MAX_WKRZESLE="$(awk '/^#define[[:space:]]+LICZBA_RZEDOW[[:space:]]/{r=$3} /^#define[[:space:]]+KRZESLA_W_RZEDZIE[[:space:]]/{s=$3} END{ if(r>0 && s>0) printf "%d", (r/2)*s; else print "36" }' "$APP_DIR/config.h")"

# Krótszy bieg + limity żeby nie rozjechać liczb
run_main_bg 60 25 2500 200
PID_MAIN="$RUN_MAIN_PID"

min=999999
max=-999999
viol=""
viol_snap=""

# sample co 0.2s dopóki main żyje
while kill -0 "$PID_MAIN" 2>/dev/null; do
  snap="$(cd "$APP_DIR" && ./monitor --once --no-color 2>/dev/null || true)"
  # jeśli nie da się czytać, to wyjdź z pętli
  [[ -z "$snap" ]] && break

  # Liczniki: teren=..  peron=..  w_krzesle=..  gora=..
  wk="$(echo "$snap" | grep -aEo "w_krzesle=-?[0-9]+" | head -n1 | cut -d= -f2 || true)"
  if [[ -n "$wk" ]]; then
    # aktualizuj min/max
    if [[ "$wk" -lt "$min" ]]; then min="$wk"; fi
    if [[ "$wk" -gt "$max" ]]; then max="$wk"; fi

    if [[ "$wk" -lt 0 || "$wk" -gt "$MAX_WKRZESLE" ]]; then
      viol="w_krzesle=$wk (limit=0..$MAX_WKRZESLE)"
      viol_snap="$snap"
      break
    fi
  fi

  sleep 0.2
done

wait_main "$PID_MAIN"
OUTDIR="$(collect_results "$TEST_NAME")"

# Dowód drenowania do zera: wyciąg loguje finalny w_krzesle
# (wymaga loga w wyciag.c: Drenowanie zakończone (w_krzesle=...)
drain_line="$(grep -aE "Drenowanie zakończone" "$OUTDIR/wyciag.log" 2>/dev/null | tail -n1 || true)"
drain_ok=0
if echo "$drain_line" | grep -aE "w_krzesle=0" >/dev/null 2>&1; then
  drain_ok=1
fi

fail=0
if [[ -n "$viol" ]]; then
  echo "[FAIL] Naruszenie zakresu: $viol" >&2
  fail=1
fi
if [[ "$drain_ok" -ne 1 ]]; then
  echo "[FAIL] Brak dowodu że po DRAINING w_krzesle=0 (sprawdź wyciag.log)" >&2
  fail=1
fi

{
  echo "# $TEST_NAME"
  echo
  echo "Cel: (1) w_krzesle nigdy nie spada poniżej 0 ani nie przekracza limitu, (2) po DRAINING jest 0." 
  echo
  echo "Limit z config.h: MAX_WKRZESLE=$MAX_WKRZESLE (= (LICZBA_RZEDOW/2)*KRZESLA_W_RZEDZIE)"
  echo "Zaobserwowane: min=$min max=$max"
  echo
  echo "## Wynik"
  if [[ -z "$viol" ]]; then
    echo "OK: w_krzesle w całym biegu mieściło się w [0..$MAX_WKRZESLE]."
  else
    echo "FAIL: $viol"
  fi
  echo
  echo "## Dowód drenowania do zera"
  echo '```'
  echo "$drain_line"
  echo '```'
  echo
  if [[ -n "$viol_snap" ]]; then
    echo "## Snapshot MONITOR przy naruszeniu"
    echo '```'
    echo "$viol_snap"
    echo '```'
    echo
  fi
  echo "## Interpretacja (do prezentacji)"
  echo "- w_krzesle to liczba zajętych miejsc na odcinku w górę, liczona w slotach (waga_slotow)."
  echo "- DRAINING kończy się dopiero gdy kolejka i ring są puste → w_krzesle=0." 
} > "$OUTDIR/summary.txt"

print_hint_screenshots "$OUTDIR"
exit "$fail"
