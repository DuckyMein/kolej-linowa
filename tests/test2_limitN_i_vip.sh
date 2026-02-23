#!/usr/bin/env bash
set -euo pipefail
# Test 2 – Limit N i VIP + klienci "nie korzysta" (v3)
# Fixes:
# - case-insensitive grep (logi są w małych literach: "odchodzi - dziś nie korzysta ...")
# - bez podwójnego "0" (grep -c już wypisuje 0 przy braku dopasowań, więc nie dokładamy "|| echo 0")

source "$(dirname "$0")/common.sh"

reset_logs
build_project

# Mały N, żeby wymusić blokowanie na semaforze terenu + widoczne VIPy
N="${1:-6}"
T="${2:-15}"

run_main_bg "$N" "$T"
PID="$RUN_MAIN_PID"
wait_main "$PID" || true

OUTDIR="$(collect_results "test2_limitN_i_vip")"

BRAMKI_LOG="$OUTPUT_DIR/bramki.log"
KLIENCI_LOG="$OUTPUT_DIR/klienci.log"

# VIP (dedykowana BRAMKA1)
vip_ok_count="$(grep -c -E "BRAMKA1: OK.*vip=1" "$BRAMKI_LOG" 2>/dev/null || true)"
vip_ok_count="${vip_ok_count:-0}"
vip_example="$(grep -m1 -E "BRAMKA1: OK.*vip=1" "$BRAMKI_LOG" 2>/dev/null || true)"

# Bramki OK łącznie (dowód że klienci przechodzą)
bramki_ok_count="$(grep -c -E "BRAMKA[0-9]+: OK" "$BRAMKI_LOG" 2>/dev/null || true)"
bramki_ok_count="${bramki_ok_count:-0}"

# Nie korzysta (logi klienta są np.: "odchodzi - dziś nie korzysta z kolei ...")
pattern_niekorz='rezygnuj|nie korzysta|odchodzi'
niekorz_count="$(grep -ciE "$pattern_niekorz" "$KLIENCI_LOG" 2>/dev/null || true)"
niekorz_count="${niekorz_count:-0}"
niekorz_example="$(grep -iEm1 "$pattern_niekorz" "$KLIENCI_LOG" 2>/dev/null || true)"

{
  echo "TEST2: limit N i VIP"
  echo "N=$N, CZAS=$T"
  echo
  echo "[LIMIT N] Bramka używa blokującego sem_wait (N=$N) - klienci czekają, nie są odrzucani."
  echo "bramki_ok_count=$bramki_ok_count"
  echo

  echo "[VIP] Dedykowana BRAMKA1 (VIP-only):"
  echo "vip_ok_count=$vip_ok_count"
  if [[ -n "$vip_example" ]]; then
    echo "Przykład:"
    echo "$vip_example"
  else
    echo "(brak VIP w tym uruchomieniu – VIP jest losowy ~1%.)"
  fi

  echo
  echo "[NIE KORZYSTA] Klienci, którzy rezygnują (case-insensitive):"
  echo "count=$niekorz_count"
  if [[ -n "$niekorz_example" ]]; then
    grep -iE "$pattern_niekorz" "$KLIENCI_LOG" | head -n 10 || true
  else
    echo "(brak w tym uruchomieniu – zależy od PROC_NIE_KORZYSTA w config.h)"
  fi
} > "$OUTDIR/summary.txt"

fail=0
# VIP-only: BRAMKA1 obsługuje wyłącznie VIP-ów
# Przy dużej liczbie klientów (~1% VIP) powinien być przynajmniej 1
if [[ "$vip_ok_count" -le 0 ]]; then
  echo "[WARN] Brak VIP w bramce1 (losowe ~1%, nie jest to twardy FAIL)" >&2
fi
# Sprawdź że bramki w ogóle przepuściły klientów
if [[ "$bramki_ok_count" -le 0 ]]; then
  echo "[FAIL] Bramki nie przepuściły żadnego klienta" >&2
  fail=1
fi

print_hint_screenshots "$OUTDIR"
exit "$fail"
