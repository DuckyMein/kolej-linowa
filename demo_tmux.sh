#!/usr/bin/env bash
set -euo pipefail

# Demo: uruchamia symulację i pokazuje 4 panele logów w tmux.
# Działa w WSL2 i przez SSH (nie wymaga GUI).

cd "$(dirname "$0")"

if ! command -v tmux >/dev/null 2>&1; then
  echo "[demo_tmux.sh] Brak 'tmux'. Zainstaluj: sudo apt update && sudo apt install -y tmux" >&2
  exit 1
fi

mkdir -p output

# Wyzeruj logi na start
: > output/main.log
: > output/pracownicy.log
: > output/generator.log
: > output/kasa.log
: > output/bramki.log
: > output/wyciag.log
: > output/klienci.log
: > output/sprzatacz.log

ARGS=("$@")
if [ ${#ARGS[@]} -eq 0 ]; then
  ARGS=(20 30)
fi

# Uruchom main w tle i loguj do pliku
./main "${ARGS[@]}" > output/main.log 2>&1 &
MAIN_PID=$!

SESSION="kolej_demo_${MAIN_PID}"

# Jeżeli sesja o tej nazwie już istnieje, dobij ją
if tmux has-session -t "$SESSION" >/dev/null 2>&1; then
  tmux kill-session -t "$SESSION" || true
fi

# Utwórz sesję i podziel na 4 panele (2x2)
tmux new-session -d -s "$SESSION" -n "kolej"

# Pane 1: po prawej
tmux split-window -h -t "$SESSION:0.0"
# Pane 2: lewy dół
tmux split-window -v -t "$SESSION:0.0"
# Pane 3: prawy dół
tmux split-window -v -t "$SESSION:0.1"

# Ładny układ
tmux select-layout -t "$SESSION:0" tiled >/dev/null 2>&1 || true

# Każdy panel: tail -F w tle + auto-zamknięcie gdy main się skończy.
SNIPPET="t=\$!; while kill -0 $MAIN_PID 2>/dev/null; do sleep 1; done; kill \$t 2>/dev/null; exit 0"

CMD_A="echo MAIN_PID=$MAIN_PID; echo 'A: MAIN + awarie'; tail -F output/main.log output/pracownicy.log output/sprzatacz.log & $SNIPPET"
CMD_B="echo 'B: GENERATOR'; tail -F output/generator.log & $SNIPPET"
CMD_C="echo 'C: WEJŚCIE (kasa + bramki)'; tail -F output/kasa.log output/bramki.log & $SNIPPET"
CMD_D="echo 'D: PRZEJAZD / KOLEJ'; tail -F output/wyciag.log output/klienci.log & $SNIPPET"

# Wyślij komendy do paneli
tmux send-keys -t "$SESSION:0.0" "$CMD_A" C-m
tmux send-keys -t "$SESSION:0.1" "$CMD_B" C-m
tmux send-keys -t "$SESSION:0.2" "$CMD_C" C-m
tmux send-keys -t "$SESSION:0.3" "$CMD_D" C-m

cat <<MSG
[demo_tmux.sh] Start. MAIN_PID=$MAIN_PID  (session: $SESSION)
- Zmiana panelu: Ctrl+b, potem strzałki.
- Odłącz: Ctrl+b d
- Zakończ sesję: tmux kill-session -t $SESSION
- Jeśli chcesz ubić symulację: kill $MAIN_PID
MSG

# Dołącz do sesji
tmux attach -t "$SESSION"
