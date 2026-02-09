#!/usr/bin/env bash
set -euo pipefail
# Test 1 – Limity krzesełek i zasady sadzania (HARD)
# - twarde liczenie zdarzeń BOARD/ARRIVE jako liczby WYSTĄPIEŃ wzorca "KLIENT <id>: BOARD/ARRIVE"
# - opcjonalna zgodność z "Liczba przejazdów" z raportu dziennego
# - dodatkowo: max(rozmiar_grupy) <= 4

source "$(dirname "$0")/common.sh"

reset_logs
build_project

N="${1:-120}"
T="${2:-10}"

run_main_bg "$N" "$T"
PID="$RUN_MAIN_PID"
wait_main "$PID" || true

OUTDIR="$(collect_results "test1_krzeselka_i_sloty")"

WYCIAG_LOG="$OUTPUT_DIR/wyciag.log"
KLIENCI_LOG="$OUTPUT_DIR/klienci.log"

start_line="$(grep -m1 -E "WYCIAG: Start" "$WYCIAG_LOG" 2>/dev/null || true)"
rzedow="$(echo "$start_line" | sed -nE 's/.*RZEDOW=([0-9]+).*/\1/p' || true)"
sloty="$(echo "$start_line" | sed -nE 's/.*SLOTY\/RZAD=([0-9]+).*/\1/p' || true)"

# HARD counts (liczymy wystąpienia wzorca, nie linie)
board_cnt="$(grep -aoE 'KLIENT[[:space:]]+[0-9]+:[[:space:]]+BOARD\b' "$KLIENCI_LOG" 2>/dev/null | wc -l | tr -d ' ')"
arrive_cnt="$(grep -aoE 'KLIENT[[:space:]]+[0-9]+:[[:space:]]+ARRIVE\b' "$KLIENCI_LOG" 2>/dev/null | wc -l | tr -d ' ')"

# max rozmiar_grupy w logach klienta (twardy check slotów)
max_group="$(grep -aoE 'rozmiar_grupy=[0-9]+' "$KLIENCI_LOG" 2>/dev/null | sed -E 's/.*=//' | sort -n | tail -n1 || true)"
max_group="${max_group:-?}"

# Raport (jeśli istnieje)
RAPORT_FILE=""
for f in "$OUTPUT_DIR/raport_dzienny.txt" "$OUTPUT_DIR/raport.txt" "$OUTPUT_DIR/raport.md" "$OUTPUT_DIR/raport"; do
  if [[ -f "$f" ]]; then RAPORT_FILE="$f"; break; fi
done
raport_przejazdy=""
if [[ -n "$RAPORT_FILE" ]]; then
  raport_przejazdy="$(grep -E "Liczba przejazdów:" "$RAPORT_FILE" 2>/dev/null | head -n1 | sed -E 's/.*:[[:space:]]*([0-9]+).*/\1/' || true)"
fi

{
  echo "TEST1(HARD): krzesełka i sloty"
  echo "N=$N, CZAS=$T"
  echo
  echo "[WYCIAG] Start:"
  echo "${start_line:-BRAK}"
  echo "Parsed: RZEDOW=${rzedow:-?}, SLOTY/RZAD=${sloty:-?}"
  if [[ "${rzedow:-}" == "72" && "${sloty:-}" == "4" ]]; then
    echo "OK: parametry wyciągu zgodne (72/4)."
  else
    echo "NIEOK: parametry wyciągu niezgodne (oczekiwano 72/4)."
  fi

  echo
  echo "[STAT - HARD] Zdarzenia z klienci.log (wystąpienia wzorca):"
  echo "BOARD events:  $board_cnt"
  echo "ARRIVE events: $arrive_cnt"
  if [[ "$board_cnt" -eq "$arrive_cnt" ]]; then
    echo "OK: BOARD == ARRIVE (spójne przejazdy)."
  else
    echo "UWAGA: BOARD != ARRIVE (możliwy wpływ końca dnia/kill lub nietypowy log)."
  fi

  echo
  echo "[CHECK] Max rozmiar_grupy (sloty) z klienci.log:"
  echo "max_rozmiar_grupy=$max_group"
  if [[ "$max_group" != "?" && "$max_group" -le 4 ]]; then
    echo "OK: max_rozmiar_grupy <= 4."
  else
    echo "NIEOK: wykryto rozmiar_grupy > 4 (powinno być niemożliwe)."
  fi

  echo
  if [[ -n "$raport_przejazdy" ]]; then
    echo "[RAPORT] Liczba przejazdów z raportu: $raport_przejazdy"
    if [[ "$raport_przejazdy" == "$board_cnt" && "$raport_przejazdy" == "$arrive_cnt" ]]; then
      echo "OK: raport == BOARD == ARRIVE."
    else
      echo "UWAGA: raport nie zgadza się z liczeniem BOARD/ARRIVE (sprawdź plik raportu i wzorce)."
    fi
  else
    echo "[RAPORT] Nie znaleziono raportu dziennego w output/ (pomijam porównanie)."
  fi

  echo
  echo "[SAMPLE] BOARD (5 przykładów):"
  grep -m5 -aE 'KLIENT[[:space:]]+[0-9]+:[[:space:]]+BOARD\b' "$KLIENCI_LOG" 2>/dev/null || true

  echo
  echo "[SAMPLE] ARRIVE (5 przykładów):"
  grep -m5 -aE 'KLIENT[[:space:]]+[0-9]+:[[:space:]]+ARRIVE\b' "$KLIENCI_LOG" 2>/dev/null || true

} > "$OUTDIR/summary.txt"

print_hint_screenshots "$OUTDIR"
