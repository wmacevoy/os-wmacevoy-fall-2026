CC      = cc
CFLAGS  = -std=c11 -Wall -Wextra -g -O0
TARGET  = memmap

all: $(TARGET)

$(TARGET): memmap.c
	$(CC) $(CFLAGS) -o $@ $<

run: $(TARGET)
	./$(TARGET) world

# Same program, twice: the addresses move (ASLR), the layout does not.
twice: $(TARGET)
	./$(TARGET) alice
	@echo
	./$(TARGET) bob

# Where the linker put each symbol, before the program ever runs.
symbols: $(TARGET)
	@nm -m $(TARGET) 2>/dev/null | grep -Ei 'main|greet|deeper|global_|bss_scratch' \
	  || nm $(TARGET) | grep -Ei 'main|greet|deeper|global_|bss_scratch'

# Section sizes in the executable image (note: .bss costs 0 bytes on disk).
sizes: $(TARGET)
	@size -m $(TARGET) 2>/dev/null || size $(TARGET)

clean:
	rm -rf $(TARGET) $(TARGET).dSYM

.PHONY: all run twice symbols sizes clean
