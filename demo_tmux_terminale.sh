#!/usr/bin/env bash
set -euo pipefail

# Wrapper: nazwa pod WSL2/SSH ("terminale"), a w środku używa układu tmux.
cd "$(dirname "$0")"
exec ./demo_tmux.sh "$@"
