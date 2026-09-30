COMMON_FLAGS = -Wall -Wextra -g -O3
CXXFLAGS = -std=c++17 $(COMMON_FLAGS)

# sizes to time, e.g. make run N="1000 1000000" REPS=5
N    = 1000 10000 100000 1000000
REPS = 3

all: bench hft stats

bench : bench.cpp
hft : hft.cpp
stats : stats.cpp

run: bench
	./bench --reps $(REPS) $(N)
	python3 bench.py --reps $(REPS) $(N) | tail -n +2
	node bench.js --reps $(REPS) $(N) | tail -n +2

clean:
	rm -rf bench hft stats *.dSYM

.PHONY: all run clean
