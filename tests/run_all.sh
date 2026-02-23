#!/usr/bin/env bash
set -uo pipefail

cd "$(dirname "$0")"

chmod +x ./*.sh || true

TESTS=(
  test1_krzeselka_i_sloty
  test2_limitN_i_vip
  test3_awaria_stop_start
  test4_koniec_dnia_drenowanie
  test5_komunikacja_end_to_end
  test6_skrajnosci_sigstop_kolejki
  test7_sigterm_klient_na_peronie_semundo
  test8_crash_main_sprzatacz_cleanup
  test9_wkrzesle_range_i_drain_zero
)

total=${#TESTS[@]}
passed=0
failed=0
failed_names=()

echo "== RUN ALL TESTS ($total) =="
echo

for t in "${TESTS[@]}"; do
  echo "--- $t ---"
  if ./"${t}.sh"; then
    echo "[PASS] $t"
    ((passed++))
  else
    echo "[FAIL] $t"
    ((failed++))
    failed_names+=("$t")
  fi
  echo
done

echo "=============================="
echo "WYNIKI: $passed/$total PASS, $failed/$total FAIL"
if [[ "$failed" -gt 0 ]]; then
  echo "Failujące testy:"
  for f in "${failed_names[@]}"; do
    echo "  - $f"
  done
  echo "=============================="
  exit 1
else
  echo "=============================="
  exit 0
fi
