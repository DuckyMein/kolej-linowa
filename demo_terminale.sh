#!/usr/bin/env bash
set -euo pipefail

# Demo: uruchamia symulację i (best-effort) odpala 4 okna terminala z tail -F.
# Wymaga GUI + terminal emulator (gnome-terminal/konsole/xfce4-terminal/xterm).
# Jeśli nie ma GUI (WSL2/SSH) i jest tmux, uruchomi ./demo_tmux.sh.

cd "$(dirname "$0")"

mkdir -p output

# Argumenty do main (jeśli nie podasz, weź sensowne demo)
ARGS=("$@")
if [ ${#ARGS[@]} -eq 0 ]; then
  ARGS=(20 30)
fi

# Wybierz emulator terminala (linux GUI)
TERM_CMD=""
if command -v gnome-terminal >/dev/null 2>&1; then
  TERM_CMD="gnome-terminal"
elif command -v konsole >/dev/null 2>&1; then
  TERM_CMD="konsole"
elif command -v xfce4-terminal >/dev/null 2>&1; then
  TERM_CMD="xfce4-terminal"
elif command -v xterm >/dev/null 2>&1; then
  TERM_CMD="xterm"
fi

HAS_GUI=0
if [ -n "$TERM_CMD" ] && { [ -n "${DISPLAY:-}" ] || [ -n "${WAYLAND_DISPLAY:-}" ]; }; then
  HAS_GUI=1
fi

# Fallback: WSL2/SSH -> tmux
if [ "$HAS_GUI" -eq 0 ] && command -v tmux >/dev/null 2>&1 && [ -x "./demo_tmux.sh" ]; then
  exec ./demo_tmux.sh "${ARGS[@]}"
fi

# Wyzeruj logi na start
: > output/main.log
: > output/pracownicy.log
: > output/generator.log
: > output/kasa.log
: > output/bramki.log
: > output/wyciag.log
: > output/klienci.log
: > output/sprzatacz.log

# Uruchom main w tle i loguj do pliku
./main "${ARGS[@]}" > output/main.log 2>&1 &
MAIN_PID=$!

open_tail() {
  local title="$1"; shift
  local files=("$@")

  local cmd="cd '$PWD'; tail -F ${files[*]}"

  case "$TERM_CMD" in
    gnome-terminal)
      (gnome-terminal --title="$title" -- bash -lc "$cmd" >/dev/null 2>&1 &) ;;
    konsole)
      (konsole --new-tab --title "$title" -e bash -lc "$cmd" >/dev/null 2>&1 &) ;;
    xfce4-terminal)
      (xfce4-terminal --title="$title" --command "bash -lc \"$cmd\"" >/dev/null 2>&1 &) ;;
    xterm)
      (xterm -T "$title" -e bash -lc "$cmd" >/dev/null 2>&1 &) ;;
    *)
      return 1 ;;
  esac
}

if [ "$HAS_GUI" -eq 1 ]; then
  open_tail "A: MAIN + awarie" output/main.log output/pracownicy.log output/sprzatacz.log || true
  open_tail "B: GENERATOR" output/generator.log || true
  open_tail "C: WEJŚCIE (kasa + bramki)" output/kasa.log output/bramki.log || true
  open_tail "D: PRZEJAZD / KOLEJ" output/wyciag.log output/klienci.log || true

  echo "[demo_terminale.sh] Uruchomiono terminale ($TERM_CMD). MAIN_PID=$MAIN_PID"
else
  echo "[demo_terminale.sh] Nie wykryto środowiska GUI/terminala. MAIN_PID=$MAIN_PID"
  echo "Otwórz ręcznie (z katalogu projektu):"
  echo "  tail -F output/main.log output/pracownicy.log output/sprzatacz.log"
  echo "  tail -F output/generator.log"
  echo "  tail -F output/kasa.log output/bramki.log"
  echo "  tail -F output/wyciag.log output/klienci.log"
  echo
  echo "Tip (WSL2/SSH): zainstaluj tmux i uruchom: ./demo_tmux.sh ${ARGS[*]}"
fi

wait "$MAIN_PID" || true

echo "[demo_terminale.sh] Koniec. Raport: output/raport_dzienny.txt"
