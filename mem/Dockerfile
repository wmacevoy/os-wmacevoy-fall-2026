# Linux with clang and LLVM's OpenMP runtime (libomp), for hft_omp.
#
# Native platform on purpose (arm64 on Apple Silicon): this is a benchmark,
# and x86 emulation would be many times slower. Docker Desktop gives the
# container the VM's cores (Settings > Resources); OpenMP uses them all
# unless OMP_NUM_THREADS or --threads says otherwise.
FROM debian:trixie-slim

RUN apt-get update \
 && apt-get install -y --no-install-recommends clang libomp-dev make \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY hft_omp.cpp Makefile ./
RUN make hft_omp CXX=clang++

ENTRYPOINT ["/src/hft_omp"]
