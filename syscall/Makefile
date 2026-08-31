# The barrier, by hand and on the clock.
CC     = cc
# _POSIX_C_SOURCE is not decoration: -std=c11 alone is strict ISO C, and on
# glibc that HIDES clock_gettime, which is POSIX, not C. Apple's headers leak
# it regardless, so the container is where you find this out. Fitting, for a
# demo about where the boundary is drawn.
CFLAGS = -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -O0 -g

all: raw cost

# No libc on the interesting path: raw.c makes its own syscalls. We still link
# the runtime for _start, because writing that too is a different lecture.
raw: raw.c
	$(CC) $(CFLAGS) -DRAW_MAIN -o $@ raw.c

cost: cost.c raw.c
	$(CC) $(CFLAGS) -o $@ cost.c raw.c

run: all
	@./raw
	@echo
	@./cost

# Part 1's real payoff: find `svc`/`syscall` in YOUR binary, then find the same
# instruction inside libc's write(). Same instruction. One of them you wrote.
disasm: raw
	@echo "--- the trap instruction in raw ---"
	@(objdump -d raw 2>/dev/null || otool -tV raw) | grep -E 'svc|syscall' | head

clean:
	rm -f raw cost
.PHONY: all run disasm clean
