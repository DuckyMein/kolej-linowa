#!/usr/bin/env bash
set -euo pipefail
# Test 2 – Limit N i VIP + klienci "nie korzysta" (v3)
# Fixes:
# - case-insensitive grep (logi są w małych literach: "odchodzi - dziś nie korzysta ...")
# - bez podwójnego "0" (grep -c już wypisuje 0 przy braku dopasowań, więc nie dokładamy "|| echo 0")

source "$(dirname "$0")/common.sh"

reset_logs
build_project

# Mały N, żeby wymusić odrzuty przez limit terenu
N="${1:-6}"
T="${2:-10}"

run_main_bg "$N" "$T"
PID="$RUN_MAIN_PID"
wait_main "$PID" || true

OUTDIR="$(collect_results "test2_limitN_i_vip")"

BRAMKI_LOG="$OUTPUT_DIR/bramki.log"
KLIENCI_LOG="$OUTPUT_DIR/klienci.log"

# Limit N (odrzuty)
odrzut_count="$(grep -c -E "ODRZUT.*brak miejsca" "$BRAMKI_LOG" 2>/dev/null || true)"
odrzut_count="${odrzut_count:-0}"

# VIP (dedykowana BRAMKA1)
vip_ok_count="$(grep -c -E "BRAMKA1: OK.*vip=1" "$BRAMKI_LOG" 2>/dev/null || true)"
vip_ok_count="${vip_ok_count:-0}"
vip_example="$(grep -m1 -E "BRAMKA1: OK.*vip=1" "$BRAMKI_LOG" 2>/dev/null || true)"

# Nie korzysta (logi klienta są np.: "odchodzi - dziś nie korzysta z kolei ...")
pattern_niekorz='rezygnuj|nie korzysta|odchodzi'
niekorz_count="$(grep -ciE "$pattern_niekorz" "$KLIENCI_LOG" 2>/dev/null || true)"
niekorz_count="${niekorz_count:-0}"
niekorz_example="$(grep -im1E "$pattern_niekorz" "$KLIENCI_LOG" 2>/dev/null || true)"

{
  echo "TEST2: limit N i VIP"
  echo "N=$N, CZAS=$T"
  echo
  echo "[LIMIT N] Odrzuty z powodu limitu terenu (powinny się pojawić przy małym N):"
  echo "count=$odrzut_count"
  grep -E "ODRZUT.*brak miejsca" "$BRAMKI_LOG" | head -n 15 || true

  echo
  echo "[VIP] Dedykowana BRAMKA1 (VIP-only):"
  echo "vip_ok_count=$vip_ok_count"
  if [[ -n "$vip_example" ]]; then
    echo "Przykład:"
    echo "$vip_example"
  else
    echo "(brak VIP w tym uruchomieniu – VIP jest losowy ~1%. Uruchom ponownie lub zwiększ czas.)"
  fi

  echo
  echo "[NIE KORZYSTA] Klienci, którzy rezygnują (case-insensitive):"
  echo "count=$niekorz_count"
  if [[ -n "$niekorz_example" ]]; then
    grep -iE "$pattern_niekorz" "$KLIENCI_LOG" | head -n 10 || true
  else
    echo "(brak w tym uruchomieniu – zależy od losowania PROC_NIE_KORZYSTA)"
  fi
} > "$OUTDIR/summary.txt"

print_hint_screenshots "$OUTDIR"
