#!/bin/bash
# Run the demo under one or both architectures.
#
#   ./run.sh                     # both, native first: same source, two conventions
#   ./run.sh arm64               # just one
#   ./run.sh amd64 make disasm   # any command in the container
#   ./run.sh --shell amd64       # a shell, to poke at it
#
# With no architecture named you get both, which is the interesting way to run
# it: two containers, two instruction sets, one identical line of output.
set -euo pipefail
cd "$(dirname "$0")"

IMAGE=${IMAGE:-os-syscall}

case "$(uname -m)" in
    arm64|aarch64) NATIVE=arm64 ;;
    x86_64|amd64)  NATIVE=amd64 ;;
    *)             NATIVE=amd64 ;;   # unknown host: everything is emulated anyway
esac

SEL=""
SHELL_MODE=0
CMD=()

while [ $# -gt 0 ]; do
    case "$1" in
        both)          SEL=both ;;
        amd64|x86_64)  SEL=amd64 ;;
        arm64|aarch64) SEL=arm64 ;;
        --shell)       SHELL_MODE=1 ;;
        -h|--help)     awk 'NR>1 && /^#/ {sub(/^# ?/,""); print; next} NR>1 {exit}' "$0"; exit 0 ;;
        --)            shift; while [ $# -gt 0 ]; do CMD+=("$1"); shift; done; break ;;
        *)             CMD+=("$1") ;;
    esac
    shift
done

# A shell in two containers at once is not a thing; default it to this host's.
if [ "$SHELL_MODE" = 1 ] && [ -z "$SEL" ]; then
    SEL=$NATIVE
fi

case "${SEL:-both}" in
    both)  ARCHES=("$NATIVE" "$([ "$NATIVE" = amd64 ] && echo arm64 || echo amd64)") ;;
    amd64) ARCHES=(amd64) ;;
    arm64) ARCHES=(arm64) ;;
esac

if ! docker info >/dev/null 2>&1; then
    echo "docker is not running -- start Docker Desktop and try again" >&2
    exit 1
fi

for arch in "${ARCHES[@]}"; do
    if ! docker image inspect "$IMAGE:$arch" >/dev/null 2>&1; then
        echo "image $IMAGE:$arch not found; building it first" >&2
        ./build.sh "$arch"
    fi
done

for arch in "${ARCHES[@]}"; do
    if [ "$arch" = "$NATIVE" ]; then
        note="native"
    else
        note="emulated on this $NATIVE host"
    fi

    echo
    echo "──── linux/$arch · $note ────"

    # cost.c times the barrier. On the emulated side it is timing translated
    # code, so say so rather than let the numbers quietly pass for measurements
    # of this machine. (How far off they are depends on the translator: Rosetta
    # lands close, qemu does not land close at all.)
    if [ "$arch" != "$NATIVE" ] && [ "$SHELL_MODE" = 0 ] && [ ${#CMD[@]} -eq 0 ]; then
        echo "     (emulated: trust raw's output and \`make disasm\` here -- the"
        echo "      instruction is the point. Take the timings from the native side.)"
    fi

    DOCKER_ARGS=(run --rm --platform "linux/$arch")

    if [ "$SHELL_MODE" = 1 ]; then
        exec docker "${DOCKER_ARGS[@]}" -it "$IMAGE:$arch" /bin/bash
    elif [ ${#CMD[@]} -eq 0 ]; then
        docker "${DOCKER_ARGS[@]}" "$IMAGE:$arch"
    else
        docker "${DOCKER_ARGS[@]}" "$IMAGE:$arch" "${CMD[@]}"
    fi
done
