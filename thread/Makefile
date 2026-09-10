CC      = cc
CFLAGS  = -std=c11 -Wall -Wextra -g -O3 -pthread
CXX      = c++
CXXFLAGS = -std=c++17 -Wall -Wextra -g -O3 -pthread
TARGET  = hello

all: $(TARGET) sum sumxx

$(TARGET): hello.c
	$(CC) $(CFLAGS) -o $@ $<

sum : sum.c
	$(CC) $(CFLAGS) -o $@ $<

sumxx : sumxx.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

run: $(TARGET)
	./$(TARGET)

# Run it a few times: the process id is one per run, the thread ids are two.
twice: $(TARGET)
	./$(TARGET)
	@echo
	./$(TARGET)

clean:
	rm -rf $(TARGET) sum sumxx *.dSYM

.PHONY: all run twice clean
