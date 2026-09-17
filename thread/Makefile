COMMON_FLAGS = -Wall -Wextra -g -O3 -pthread
CFLAGS  = -std=c11 $(COMMON_FLAGS)
CXXFLAGS = -std=c++17 $(COMMON_FLAGS)
LDLIBS  = -lm

all: hello sum sumxx factory factoryxx factoryxx_guarded

hello: hello.c
philosophers : philosophers.c
sum : sum.c
sumxx : sumxx.cpp
factory : factory.c
factoryxx : factoryxx.cpp

# The built-in rule would hand the header to the compiler too.
factoryxx_guarded : factoryxx_guarded.cpp guarded.hpp
	$(LINK.cpp) $< $(LDLIBS) -o $@

clean:
	rm -rf hello sum sumxx philosophers factory factoryxx factoryxx_guarded *.dSYM

.PHONY: all run clean

