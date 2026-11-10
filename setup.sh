#!/usr/bin/env bash
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
if [[ ${1:-} == --help || ${1:-} == -h ]]; then
    cat <<'EOF'
Usage: ./setup.sh [--position absolute|relative] [--width 800 --height 600]
                  [--match-class CLASS]
                  [--startup-delay 1.0] [--click-delay 0.25]
                  [--timeout 20] -- PROGRAM [ARGUMENTS...]

With no arguments, prompts for the program, position mode, and window size.
Absolute places the window at desktop (0, 0); relative keeps its launch position.
Both modes apply the specified size. Relative is the default.
Launches the program, records and executes left clicks, then generates
generated/config.h. Escape finishes. Run make to build the executable.
Uses native Hyprland clicks; ydotool and a privileged daemon are not needed.
EOF
    exit 0
fi

missing=()
command -v cc >/dev/null || missing+=(gcc)
command -v make >/dev/null || missing+=(make)
command -v python3 >/dev/null || missing+=(python)
command -v slurp >/dev/null || missing+=(slurp)
if ((${#missing[@]})); then
    if ! command -v pacman >/dev/null; then
        printf 'Install these dependencies, then retry: %s\n' "${missing[*]}" >&2
        exit 1
    fi
    printf 'Missing packages: %s\nInstall with pacman? [y/N] ' "${missing[*]}"
    read -r answer
    [[ $answer == y || $answer == Y ]] || exit 1
    sudo pacman -S --needed "${missing[@]}"
fi

exec python3 "$root/tools/record.py" "$@"
