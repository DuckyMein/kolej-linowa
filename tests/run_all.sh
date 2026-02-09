#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

chmod +x ./*.sh || true

echo "== RUN ALL TESTS =="
./test1_krzeselka_i_sloty.sh
./test2_limitN_i_vip.sh
./test3_awaria_stop_start.sh
./test4_koniec_dnia_drenowanie.sh
echo "== DONE =="
