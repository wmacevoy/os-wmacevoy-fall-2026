# x86-64 Linux, so everyone sees the same ABI regardless of the host machine.
# On an Apple Silicon Mac this runs under emulation (qemu via Docker Desktop);
# it is slow to start and perfectly fine for this program.
FROM --platform=linux/amd64 debian:bookworm-slim

RUN apt-get update \
 && apt-get install -y --no-install-recommends gcc libc6-dev make binutils util-linux \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY memmap.c Makefile ./
RUN make

ENTRYPOINT ["/src/memmap"]
CMD ["world"]
