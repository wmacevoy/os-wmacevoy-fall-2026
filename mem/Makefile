COMMON_FLAGS = -Wall -Wextra -g -O3
CXXFLAGS = -std=c++17 $(COMMON_FLAGS)

# sizes to time, e.g. make run N="1000 1000000" REPS=5
N    = 1000 10000 100000 1000000
REPS = 3

all: bench hft stats

bench : bench.cpp
hft : hft.cpp
stats : stats.cpp

# OpenMP: clang/gcc on Linux take -fopenmp. Apple clang has no OpenMP runtime
# and refuses -fopenmp unless passed through to the preprocessor, so use
# Homebrew's libomp (brew install libomp).
ifeq ($(shell uname),Darwin)
LIBOMP   ?= $(shell brew --prefix libomp 2>/dev/null)
OMPFLAGS  = -Xpreprocessor -fopenmp -I$(LIBOMP)/include -L$(LIBOMP)/lib -lomp
else
OMPFLAGS  = -fopenmp
endif

hft_omp : hft_omp.cpp
	$(CXX) $(CXXFLAGS) $(OMPFLAGS) -o $@ $<

# hft_omp in a Linux container (see Dockerfile), e.g.
#   make omp-run ARGS="--threads 4 --ticks 4000000"
IMAGE = os-mem-hft-omp
omp-image : Dockerfile hft_omp.cpp Makefile
	docker build -t $(IMAGE) .

omp-run : omp-image
	docker run --rm $(IMAGE) $(ARGS)

run: bench
	./bench --reps $(REPS) $(N)
	python3 bench.py --reps $(REPS) $(N) | tail -n +2
	node bench.js --reps $(REPS) $(N) | tail -n +2

clean:
	rm -rf bench hft hft_omp stats *.dSYM

.PHONY: all run clean omp-image omp-run
