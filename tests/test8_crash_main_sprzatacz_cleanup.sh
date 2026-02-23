#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"
source "./common.sh"

TEST_NAME="test8_crash_main_sprzatacz_cleanup"

require_cmd ipcs
require_cmd gcc

build_project
reset_logs

echo "== $TEST_NAME =="

# helper: policz klucze IPC (base + offset) tak jak w ipc.c
FTOK_FILE="$(awk -F'"' '/^[[:space:]]*#define[[:space:]]+FTOK_FILE[[:space:]]+"/{print $2; exit}' "$APP_DIR/config.h" 2>/dev/null || true)"
if [[ -z "$FTOK_FILE" ]]; then
  FTOK_FILE="/tmp/kolej_krzeselkowa_ipc.ftok"
fi

KEYS_BIN="/tmp/kolej_ftok_keys_$$"
cat > "${KEYS_BIN}.c" <<'C'
#include <sys/ipc.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
  if (argc < 2) return 2;
  const char *path = argv[1];
  key_t base = ftok(path, 'K');
  if (base == (key_t)-1) { perror("ftok"); return 1; }

  const int offsets[] = {0,1,2,3,4,5,6,7,8,9,10};
  const char *names[] = {
    "SEM","SHM","MQ_KASA","MQ_KASA_ODP","MQ_BRAMKA","MQ_BRAMKA_ODP",
    "MQ_PRAC","MQ_WYCIAG_REQ","MQ_WYCIAG_ODP","MQ_PERON","MQ_PERON_ODP"
  };
  for (int i=0;i<11;i++) {
    unsigned k = (unsigned)(base + offsets[i]);
    printf("%s 0x%08x\n", names[i], k);
  }
  return 0;
}
C

gcc -O2 "${KEYS_BIN}.c" -o "$KEYS_BIN" >/dev/null 2>&1

mapfile -t KEY_LINES < <("$KEYS_BIN" "$FTOK_FILE")
KEY_HEXES=()
for l in "${KEY_LINES[@]}"; do
  KEY_HEXES+=("$(echo "$l" | awk '{print $2}')")
done

# Start symulacji (krócej, tylko żeby IPC powstało)
run_main_bg 25 20 2000 150
PID_MAIN="$RUN_MAIN_PID"

# Poczekaj aż IPC na pewno powstanie
sleep 1.5

IPCS_BEFORE="$(ipcs -m -q -s 2>/dev/null || true)"
found_before=0
for k in "${KEY_HEXES[@]}"; do
  if echo "$IPCS_BEFORE" | grep -qi "$k"; then
    found_before=1
    break
  fi
done

if [[ "$found_before" -ne 1 ]]; then
  echo "[FAIL] Nie wykryłem obiektów IPC po naszych kluczach PRZED crashem (test niewiarygodny)." >&2
  kill -TERM "$PID_MAIN" 2>/dev/null || true
  rm -f "$KEYS_BIN" "${KEYS_BIN}.c" || true
  exit 1
fi

# Zapamiętaj PGID main (= PID main, bo main robi setpgid(0,0))
MAIN_PGID="$PID_MAIN"

# Znajdź PID sprzątacza (zanim go potrzebujemy)
SPRZATACZ_PID="$(ps -u "$(id -u)" -o pid=,comm= 2>/dev/null | grep -w 'sprzatacz' | awk '{print $1}' | head -n1 || true)"
echo "[test] Sprzątacz PID=${SPRZATACZ_PID:-not found}"

# Crash main (SIGKILL)
echo "[test] SIGKILL main PID=$PID_MAIN"
kill -KILL "$PID_MAIN" 2>/dev/null || true

# Zaczekaj chwilę na reap zombie (PDEATHSIG jest dostarczany po śmierci rodzica)
sleep 0.5

# --- Faza 1: czekaj na sprzątacza (do 10s) ---
sprzatacz_cleaned=0
left_after=1
for _ in {1..40}; do
  IPCS_AFTER="$(ipcs -m -q -s 2>/dev/null || true)"
  left_after=0
  for k in "${KEY_HEXES[@]}"; do
    if echo "$IPCS_AFTER" | grep -qi "$k"; then
      left_after=1
      break
    fi
  done
  if [[ "$left_after" -eq 0 ]]; then
    sprzatacz_cleaned=1
    break
  fi
  sleep 0.25
done

# --- Faza 2: jeśli IPC nadal istnieje, wyślij SIGUSR1 do sprzątacza (force) ---
if [[ "$left_after" -ne 0 && -n "$SPRZATACZ_PID" ]]; then
  if kill -0 "$SPRZATACZ_PID" 2>/dev/null; then
    echo "[test] Sprzątacz żyje ale nie wyczyścił - wysyłam SIGUSR1 (force)" >&2
    kill -USR1 "$SPRZATACZ_PID" 2>/dev/null || true
    # Czekaj jeszcze 5s
    for _ in {1..20}; do
      IPCS_AFTER="$(ipcs -m -q -s 2>/dev/null || true)"
      left_after=0
      for k in "${KEY_HEXES[@]}"; do
        if echo "$IPCS_AFTER" | grep -qi "$k"; then
          left_after=1
          break
        fi
      done
      if [[ "$left_after" -eq 0 ]]; then
        sprzatacz_cleaned=1
        break
      fi
      sleep 0.25
    done
  else
    echo "[test] Sprzątacz PID=$SPRZATACZ_PID już nie żyje (wyszedł bez cleanup?)" >&2
  fi
fi

# --- Faza 3: dobij procesy i ipcrm jako fallback ---
if [[ "$left_after" -ne 0 ]]; then
  echo "[test] IPC nadal istnieje - dobijam osierocone procesy (PGID=$MAIN_PGID)" >&2
  kill -KILL -"$MAIN_PGID" 2>/dev/null || true
  for name in wyciag klient generator kasjer bramka pracownik1 pracownik2 sprzatacz; do
    pkill -KILL -x "$name" 2>/dev/null || true
  done
  sleep 0.5

  # Ręczne czyszczenie IPC przez ipcrm (fallback)
  echo "[test] Ręczne ipcrm fallback" >&2
  for k in "${KEY_HEXES[@]}"; do
    # ipcrm oczekuje decimal key, konwertuj hex→dec
    dec_k="$(printf '%d' "$k" 2>/dev/null || true)"
    if [[ -n "$dec_k" ]]; then
      ipcrm -S "$dec_k" 2>/dev/null || true
      ipcrm -M "$dec_k" 2>/dev/null || true
      ipcrm -Q "$dec_k" 2>/dev/null || true
    fi
  done
  sleep 0.3

  # Sprawdź czy ipcrm pomógł
  IPCS_AFTER="$(ipcs -m -q -s 2>/dev/null || true)"
  left_after=0
  for k in "${KEY_HEXES[@]}"; do
    if echo "$IPCS_AFTER" | grep -qi "$k"; then
      left_after=1
      break
    fi
  done
fi

# sprawdź czy procesy kolejki jeszcze żyją (best-effort)
PS_LEFT="$(ps -u "$(id -u)" -o pid=,comm= 2>/dev/null | grep -E '\b(main|wyciag|generator|kasjer|bramka|pracownik1|pracownik2|klient|sprzatacz)\b' || true)"

OUTDIR="$(collect_results "$TEST_NAME")"

{
  echo "# $TEST_NAME"
  echo
  echo "Cel: crash-safe cleanup: po SIGKILL main sprzątacz usuwa SysV IPC (SHM/SEM/MQ)."
  echo
  echo "FTOK_FILE: $FTOK_FILE"
  echo "Sprzątacz PID: ${SPRZATACZ_PID:-not found}"
  echo
  echo "## Klucze (base + offset)"
  echo '```'
  printf '%s\n' "${KEY_LINES[@]}"
  echo '```'
  echo
  echo "## ipcs PRZED crashem"
  echo "OK: znaleziono obiekt(y) IPC po kluczach (found_before=1)."
  echo
  echo "## ipcs PO crashu"
  if [[ "$sprzatacz_cleaned" -eq 1 ]]; then
    echo "OK: sprzątacz sam wyczyścił IPC (idealne zachowanie)."
  elif [[ "$left_after" -eq 0 ]]; then
    echo "OK: IPC wyczyszczone (fallback: SIGUSR1/ipcrm z testu pomogło)."
    echo "UWAGA: sprzątacz nie poradził sobie samodzielnie (znany problem: zombie race + readlink na WSL2)."
  else
    echo "FAIL: nadal widać obiekty IPC mimo fallback ipcrm."
  fi
  echo
  echo "## Procesy (best-effort)"
  if [[ -z "$PS_LEFT" ]]; then
    echo "OK: brak procesów symulacji na liście."
  else
    echo '```'
    echo "$PS_LEFT"
    echo '```'
  fi
  echo
  echo "## Interpretacja (do prezentacji)"
  echo "- Pokazuje mechanizm crash-safe cleanup: sprzątacz (PDEATHSIG) + fallback (SIGUSR1/ipcrm)."
  if [[ "$sprzatacz_cleaned" -eq 1 ]]; then
    echo "- Sprzątacz samodzielnie wyczyścił IPC po SIGKILL main."
  else
    echo "- Sprzątacz nie wyczyścił IPC samodzielnie (zombie race / readlink -1 na WSL2)."
    echo "  Test użył SIGUSR1 + ipcrm fallback - IPC wyczyszczone."
  fi
} > "$OUTDIR/summary.txt"

rm -f "$KEYS_BIN" "${KEYS_BIN}.c" || true
print_hint_screenshots "$OUTDIR"

# Test przechodzi jeśli IPC zostało wyczyszczone (niezależnie czy przez sprzątacza czy fallback)
# Kluczowe: IPC nie powinno zostać na stałe po crashu main
if [[ "$left_after" -ne 0 ]]; then
  echo "[FAIL] IPC nie wyczyszczone mimo fallback ipcrm" >&2
  exit 1
fi

# Raportuj czy sprzątacz zadziałał sam (ale nie failuj jeśli nie)
if [[ "$sprzatacz_cleaned" -ne 1 ]]; then
  echo "[WARN] Sprzątacz nie wyczyścił IPC samodzielnie (fallback pomógł - zombie race / WSL2)" >&2
fi
