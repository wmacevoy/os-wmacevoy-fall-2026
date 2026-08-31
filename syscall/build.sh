#!/bin/bash
# Build the demo twice, once per architecture.
#
# One of the two is your machine's own ISA and compiles at full speed. The
# other is emulated, through whatever binfmt handler your Docker has: Rosetta
# on an Apple Silicon Mac with it enabled (fast), qemu otherwise (faithful but
# slow -- expect a few minutes, since apt and gcc are then running every
# instruction through a translator).
#
#   ./build.sh          # both
#   ./build.sh amd64    # just x86-64
#   ./build.sh arm64    # just arm64
set -euo pipefail
cd "$(dirname "$0")"

IMAGE=${IMAGE:-os-syscall}

case "$(uname -m)" in
    arm64|aarch64) NATIVE=arm64 ;;
    x86_64|amd64)  NATIVE=amd64 ;;
    *)             NATIVE="" ;;
esac

case "${1:-both}" in
    both)          if [ "$NATIVE" = arm64 ]; then WANT=(arm64 amd64); else WANT=(amd64 arm64); fi ;;
    amd64|x86_64)  WANT=(amd64) ;;
    arm64|aarch64) WANT=(arm64) ;;
    *) echo "usage: $(basename "$0") [both|amd64|arm64]" >&2; exit 2 ;;
esac

if ! docker info >/dev/null 2>&1; then
    echo "docker is not running -- start Docker Desktop and try again" >&2
    exit 1
fi

for arch in "${WANT[@]}"; do
    if [ "$arch" = "$NATIVE" ]; then
        note="native"
    else
        note="emulated on this ${NATIVE:-$(uname -m)} host -- slow, be patient"
    fi

    echo
    echo "==> $IMAGE:$arch   (linux/$arch, $note)"

    if ! docker build --platform "linux/$arch" -t "$IMAGE:$arch" . ; then
        echo >&2
        echo "build for linux/$arch failed." >&2
        if [ "$arch" != "$NATIVE" ]; then
            cat >&2 <<'HINT'

That is the emulated architecture, so the usual cause is a missing binfmt
handler rather than anything wrong with the code. Install the qemu handlers:

    docker run --privileged --rm tonistiigi/binfmt --install all

On Docker Desktop, also check Settings -> General that the containerd image
store is enabled. Then try again.
HINT
        fi
        exit 1
    fi
done

echo
echo "built: ${WANT[*]/#/$IMAGE:}"
echo "run them with ./run.sh"
