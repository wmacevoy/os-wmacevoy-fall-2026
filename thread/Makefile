COMMON_FLAGS = -Wall -Wextra -g -O3 -pthread
CFLAGS  = -std=c11 $(COMMON_FLAGS)
CXXFLAGS = -std=c++17 $(COMMON_FLAGS)
LDLIBS  = -lm

all: hello sum sumxx factory factoryxx

hello: hello.c
philosophers : philosophers.c
sum : sum.c
sumxx : sumxx.cpp
factory : factory.c
factoryxx : factoryxx.cpp

clean:
	rm -rf hello sum sumxx philosophers factory factoryxx *.dSYM

.PHONY: all run clean

