# One Dockerfile, two architectures.
#
# Nothing here pins an ISA. `docker build --platform linux/amd64` and
# `--platform linux/arm64` each pull their own base image, their own gcc, and
# produce their own binary -- from the same raw.c. Whichever platform is not
# your host's runs under emulation, wired in by a binfmt handler -- Rosetta on
# an Apple Silicon Mac with it enabled, qemu otherwise; see build.sh.
#
# That is the demo, not an accident of packaging. The #if blocks in raw.c are a
# survey of arbitrary conventions, and this is how you see both of them run:
#
#   linux/amd64   number in rax   `syscall`   write is 1
#   linux/arm64   number in x8    `svc #0`    write is 64
#
# Same source. Same output line. Different instruction -- `make disasm` inside
# each container is the proof.
FROM debian:bookworm-slim

RUN apt-get update \
 && apt-get install -y --no-install-recommends gcc libc6-dev make binutils \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY raw.c cost.c Makefile ./
RUN make all

# Recorded at build time so you can tell the two images apart from the inside.
ARG TARGETPLATFORM
RUN echo "${TARGETPLATFORM:-unknown} ($(uname -m))" > /src/.platform

# No ENTRYPOINT: `docker run IMAGE` gives the demo, `docker run IMAGE anything`
# gives you anything. run.sh relies on both.
CMD ["make", "--no-print-directory", "run"]
