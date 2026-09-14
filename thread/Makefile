COMMON_FLAGS = -Wall -Wextra -g -O3 -pthread
CFLAGS  = -std=c11 $(COMMON_FLAGS)
CXXFLAGS = -std=c++17 $(COMMON_FLAGS)
LDLIBS  = -lm

all: hello sum sumxx

hello: hello.c
philosophers : philosophers.c
sum : sum.c
sumxx : sumxx.cpp

clean:
	rm -rf hello sum sumxx philosophers *.dSYM

.PHONY: all run clean

