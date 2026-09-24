#!/usr/bin/env bash
# Host side of the Linux release: build the builder image and run
# build_appimage.sh in it. Runs on Linux or WSL (Docker required).
#
#   make_appimage.sh --version X.Y.Z --game DIR --engine DIR --ui DIR --out DIR \
#                    [--private DIR] [--jobs N]
#
# --private (optional) holds gba_bios.bin + emerald_usa.gba for the smoke
# test; it is mounted read-only and never enters the image.
set -euo pipefail
VERSION="" GAME="" ENGINE="" UI="" OUT="" PRIVATE="" JOBS=8
while [[ $# -gt 0 ]]; do
    case "$1" in
        --version) VERSION="$2"; shift 2 ;;
        --game) GAME="$2"; shift 2 ;;
        --engine) ENGINE="$2"; shift 2 ;;
        --ui) UI="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        --private) PRIVATE="$2"; shift 2 ;;
        --jobs) JOBS="$2"; shift 2 ;;
        *) echo "unknown argument $1"; exit 2 ;;
    esac
done
for v in VERSION GAME ENGINE UI OUT; do
    [[ -n "${!v}" ]] || { echo "--${v,,} is required"; exit 2; }
done
IMAGE=emeraldrecomp-linux-builder
docker build -q -t "$IMAGE" "$GAME/tools/linux" >/dev/null
mkdir -p "$OUT"
# Build tree on the Linux filesystem (fast, persistent between releases).
CACHE="${EMERALDRECOMP_BUILD_CACHE:-$HOME/.cache/emeraldrecomp-release}"
mkdir -p "$CACHE"
MOUNTS=(-v "$GAME:/src/game:ro" -v "$ENGINE:/src/engine:ro" -v "$UI:/src/ui:ro" -v "$OUT:/out")
if [[ -n "$PRIVATE" ]]; then MOUNTS+=(-v "$PRIVATE:/private:ro"); fi
# Build as the invoking user so /out stays owned by them.
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -e VERSION="$VERSION" -e JOBS="$JOBS" \
    -v "$CACHE:/build" "${MOUNTS[@]}" "$IMAGE" \
    bash /src/game/tools/linux/build_appimage.sh
