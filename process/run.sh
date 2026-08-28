#!/bin/bash
# Run memmap inside the x86-64 Linux image built by ./build.sh
#
#   ./run.sh                 # ./memmap world
#   ./run.sh Warren          # ./memmap Warren
#   ./run.sh --no-aslr       # kernel randomization off (the stack base still
#                            # jitters a little: see README, "Reproducibility")
#   ./run.sh --shell         # a shell in the container, to poke at it
set -euo pipefail
cd "$(dirname "$0")"

IMAGE=${IMAGE:-os-process-memmap}
NO_ASLR=0
SHELL_MODE=0
ARGS=()

for a in "$@"; do
    case "$a" in
        --no-aslr) NO_ASLR=1 ;;
        --shell)   SHELL_MODE=1 ;;
        *)         ARGS+=("$a") ;;
    esac
done

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "image $IMAGE not found; running ./build.sh first" >&2
    ./build.sh
fi

DOCKER_ARGS=(run --rm --platform linux/amd64)

if [ "$NO_ASLR" = 1 ]; then
    # setarch -R clears ADDR_NO_RANDOMIZE via the personality(2) syscall, which
    # docker's default seccomp profile blocks -- hence unconfined.
    DOCKER_ARGS+=(--security-opt seccomp=unconfined)
fi

if [ "$SHELL_MODE" = 1 ]; then
    exec docker "${DOCKER_ARGS[@]}" -it --entrypoint /bin/bash "$IMAGE"
fi

set -- "${ARGS[@]:-world}"

if [ "$NO_ASLR" = 1 ]; then
    exec docker "${DOCKER_ARGS[@]}" --entrypoint setarch "$IMAGE" -R /src/memmap "$@"
else
    exec docker "${DOCKER_ARGS[@]}" "$IMAGE" "$@"
fi
