#!/bin/bash
# Build the x86-64 Linux image. Run this once (and again after editing memmap.c).
set -euo pipefail
cd "$(dirname "$0")"

if ! docker info >/dev/null 2>&1; then
    echo "docker is not running -- start Docker Desktop and try again" >&2
    exit 1
fi

IMAGE=${IMAGE:-os-process-memmap}

echo "building $IMAGE for linux/amd64 (emulated on an arm64 host)..."
docker build --platform linux/amd64 -t "$IMAGE" .

echo
echo "built $IMAGE -- run it with ./run.sh [name]"
