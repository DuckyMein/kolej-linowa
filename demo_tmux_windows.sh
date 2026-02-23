#!/usr/bin/env bash
set -euo pipefail

# demo_tmux_windows.sh
# Tekstowe demo na Debianie (bez GUI): TMUX z osobnymi OKNAMI (nie panelami)
# RUN / MONITOR / TIMELINE / KASA / BRAMKI / WYCIAG / PRAC / KLIENCI / GEN+SPRZ / CONTROL

cd "$(dirname "${BASH_SOURCE[0]}")"
PROJ_DIR="$PWD"

need(){
  command -v "$1" >/dev/null 2>&1 || {
    echo "[demo_tmux_windows.sh] Brak '$1'" >&2
    exit 1
  }
}

need tmux
need stdbuf
need awk
need tail

mkdir -p output

# Upewnij się, że logi istnieją (tail -F nie lubi brakujących plików)
for f in main pracownicy generator kasa bramki wyciag klienci sprzatacz; do
  : > "output/${f}.log"
done

ARGS=("$@")
if [ ${#ARGS[@]} -eq 0 ]; then
  ARGS=(20 30)
fi
ARGS_STR="${ARGS[*]}"

[ -x ./main ] || { echo "[demo_tmux_windows.sh] Brak ./main (zrob: make)" >&2; exit 1; }

# Start symulacji (na zewnątrz tmux, żeby od razu mieć PID)
./main "${ARGS[@]}" > output/main.log 2>&1 &
MAIN_PID=$!

SESSION="kolej_win_${MAIN_PID}"
if tmux has-session -t "$SESSION" >/dev/null 2>&1; then
  tmux kill-session -t "$SESSION" || true
fi

# Helper: tailuje logi i kończy się, gdy MAIN_PID padnie
TAIL_HELPER="output/_tail_until_main_exit.sh"
cat > "$TAIL_HELPER" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
MAIN_PID="$1"; shift
[ "$#" -ge 1 ] || { echo "usage: _tail_until_main_exit.sh <main_pid> <file> [file...]"; exit 2; }

tail -F "$@" &
T=$!
while kill -0 "$MAIN_PID" 2>/dev/null; do sleep 1; done
kill "$T" 2>/dev/null || true
wait "$T" 2>/dev/null || true
EOF
chmod +x "$TAIL_HELPER"

# Helper: TIMELINE (filtr + kolory) — tylko kluczowe zdarzenia
TIMELINE_HELPER="output/_timeline_view.sh"
cat > "$TIMELINE_HELPER" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
MAIN_PID="$1"
cd "$(dirname "${BASH_SOURCE[0]}")/.."

LOGS=(
  output/main.log output/pracownicy.log output/generator.log
  output/kasa.log output/bramki.log output/wyciag.log output/klienci.log output/sprzatacz.log
)

# Słowa-klucze: komunikacja + fazy + awarie (celowo w ASCII, żeby było stabilnie)
KEY='(karnet|SPRZED|sprzed|BRAMK|peron|BOARD|ARRIVE|STOP|START|GOTOWY|AWARIA|PANIC|FAZA|CLOSING|DRAINING|SHUTDOWN|KONIEC|IPC|DIRTY|Ewakuacja)'

tail -F "${LOGS[@]}" \
| stdbuf -oL -eL awk -v KEY="$KEY" '
BEGIN{
  reset="\033[0m";
  red="\033[31m"; green="\033[32m"; yellow="\033[33m"; blue="\033[34m";
  mag="\033[35m"; cya="\033[36m"; wht="\033[37m";
}
function is_key(line){ return (line ~ KEY); }
function pick(line){
  if (line ~ /(AWARIA|PANIC|STOP|START|GOTOWY)/) return red;
  if (line ~ /(KLIENT|DZIECKO)/) return green;
  if (line ~ /(KASJER|kasa)/) return cya;
  if (line ~ /BRAMK/) return mag;
  if (line ~ /(WYCIAG|WYCIĄG|BOARD|ARRIVE)/) return yellow;
  if (line ~ /(GENERATOR)/) return blue;
  if (line ~ /(SPRZATACZ|Sprzatacz|IPC|DIRTY)/) return wht;
  return reset;
}
{
  if (!is_key($0)) next;
  c=pick($0);
  print c $0 reset;
  fflush();
}' &
T=$!

while kill -0 "$MAIN_PID" 2>/dev/null; do sleep 1; done
kill "$T" 2>/dev/null || true
wait "$T" 2>/dev/null || true
EOF
chmod +x "$TIMELINE_HELPER"

# --- TMUX: okna ---
tmux new-session -d -s "$SESSION" -n RUN

# RUN
tmux send-keys -t "$SESSION:RUN" \
  "cd \"$PROJ_DIR\"; echo \"MAIN_PID=$MAIN_PID\"; echo \"ARGS: $ARGS_STR\"; echo; echo \"(To okno: main.log)\"; \"$TAIL_HELPER\" \"$MAIN_PID\" output/main.log" C-m

# MONITOR
tmux new-window -t "$SESSION" -n MONITOR
if [ -x ./monitor ]; then
  tmux send-keys -t "$SESSION:MONITOR" "cd \"$PROJ_DIR\"; ./monitor --interval-ms 400" C-m
else
  tmux send-keys -t "$SESSION:MONITOR" "cd \"$PROJ_DIR\"; echo 'Brak ./monitor (zrob: make)'; sleep 999999" C-m
fi

# TIMELINE
tmux new-window -t "$SESSION" -n TIMELINE
tmux send-keys -t "$SESSION:TIMELINE" \
  "cd \"$PROJ_DIR\"; echo 'TIMELINE (kluczowe zdarzenia)'; echo; \"$TIMELINE_HELPER\" \"$MAIN_PID\"" C-m

# Procesy / logi
tmux new-window -t "$SESSION" -n KASA
tmux send-keys -t "$SESSION:KASA" "cd \"$PROJ_DIR\"; echo KASA; \"$TAIL_HELPER\" \"$MAIN_PID\" output/kasa.log" C-m

tmux new-window -t "$SESSION" -n BRAMKI
tmux send-keys -t "$SESSION:BRAMKI" "cd \"$PROJ_DIR\"; echo BRAMKI; \"$TAIL_HELPER\" \"$MAIN_PID\" output/bramki.log" C-m

tmux new-window -t "$SESSION" -n WYCIAG
tmux send-keys -t "$SESSION:WYCIAG" "cd \"$PROJ_DIR\"; echo WYCIAG; \"$TAIL_HELPER\" \"$MAIN_PID\" output/wyciag.log" C-m

tmux new-window -t "$SESSION" -n PRAC
tmux send-keys -t "$SESSION:PRAC" "cd \"$PROJ_DIR\"; echo PRACOWNICY; \"$TAIL_HELPER\" \"$MAIN_PID\" output/pracownicy.log" C-m

tmux new-window -t "$SESSION" -n KLIENCI
tmux send-keys -t "$SESSION:KLIENCI" "cd \"$PROJ_DIR\"; echo KLIENCI; \"$TAIL_HELPER\" \"$MAIN_PID\" output/klienci.log" C-m

tmux new-window -t "$SESSION" -n GEN_SPRZ
tmux send-keys -t "$SESSION:GEN_SPRZ" "cd \"$PROJ_DIR\"; echo 'GENERATOR + SPRZATACZ'; \"$TAIL_HELPER\" \"$MAIN_PID\" output/generator.log output/sprzatacz.log" C-m

# CONTROL
tmux new-window -t "$SESSION" -n CONTROL
tmux send-keys -t "$SESSION:CONTROL" "cd \"$PROJ_DIR\"; bash" C-m
tmux send-keys -t "$SESSION:CONTROL" "cat <<'HELP'
=== CONTROL: komendy demo ===

# PIDy:
pgrep -a main kasjer wyciag generator sprzatacz pracownik1 pracownik2 bramka || true

# STOP/START (jeśli obsługiwane przez main):
kill -USR1 \$(pgrep -n main); sleep 2; kill -USR2 \$(pgrep -n main)

# Skrajność: zamrożenie kasjera (patrz MONITOR/TIMELINE)
kill -STOP \$(pgrep -n kasjer); sleep 3; kill -CONT \$(pgrep -n kasjer)

# Skrajność: zamrożenie 1 bramki
PID=\$(pgrep -n bramka || true); [ -n \"\$PID\" ] && kill -STOP \"\$PID\" && sleep 3 && kill -CONT \"\$PID\"

# Szybkie wyjście (gdyby trzeba było):
kill -TERM \$(pgrep -n main) 2>/dev/null || true

HELP" C-m

tmux select-window -t "$SESSION:TIMELINE" || true
exec tmux attach -t "$SESSION"
