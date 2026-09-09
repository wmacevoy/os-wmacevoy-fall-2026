# Warnings loud and on; -g so the debugger and sanitizers can name things.
# -D_POSIX_C_SOURCE=200809L asks for the POSIX 2008 declarations (fork, execv,
# waitpid, sleep, strsignal). Without it, strict -std=c11 on glibc hides them
# and you get "implicit declaration of function 'fork'".
CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Wpedantic -g -D_POSIX_C_SOURCE=200809L

BINS = forkexec child

all: $(BINS)

forkexec: forkexec.c
	$(CC) $(CFLAGS) -o $@ $<

child: child.c
	$(CC) $(CFLAGS) -o $@ $<

# UndefinedBehaviorSanitizer. Verified working on this machine.
# Both binaries are instrumented, so the checking survives exec() into child.
sanitize: CFLAGS += -fsanitize=undefined -fno-omit-frame-pointer
sanitize: clean $(BINS)

# AddressSanitizer. KNOWN BROKEN on this machine as of 2026-08-21:
#   macOS 26.5.2 / Apple clang 16.0.0 / arm64
#   AddressSanitizer: CHECK failed: sanitizer_malloc_mac.inc:189
#                     "((!asan_init_is_running)) != (0)"
# It dies inside ASan's own startup before main() runs -- this is a runtime
# incompatibility, not a bug in our code. UBSan on the same source is fine.
# If you build on the course's Linux boxes, this target should just work; try
# it there before assuming your program is at fault.
sanitize-addr: CFLAGS += -fsanitize=address -fno-omit-frame-pointer
sanitize-addr: clean $(BINS)

# NOTE: this demo exits with the CHILD's status on purpose, which is nonzero,
# and make treats a nonzero recipe as a failed build. Reporting the status
# ourselves keeps make happy and makes the propagated value visible.
run: all
	@./forkexec; echo "[make] exit status: $$?"

# stdout becomes a pipe, so it is block-buffered instead of line-buffered.
# Nothing duplicates here -- exec() discards the inherited stdio buffer.
run-piped: all
	@./forkexec | cat

# Exercises the exec-failure path: the child survives exec(), still holding a
# copy of the parent's stdio buffers.
run-execfail: all
	-./forkexec ./no-such-program

# Reproduces the duplicated-output bug on purpose. Builds a deliberately broken
# copy with fflush(NULL) removed AND _exit() downgraded to exit(), then forces
# exec() to fail so the child exits through stdio cleanup. Watch for the
# "parent: ... forking" line appearing twice -- the second one is the child
# flushing a buffer it inherited as a copy.
demo-dup: child
	@sed -e 's/^\tfflush(NULL);/\t\/* removed *\//' \
	     -e 's/_exit(EXIT_EXEC_FAILED);/exit(EXIT_EXEC_FAILED);/' \
	     forkexec.c > broken.c
	@$(CC) $(CFLAGS) -o broken broken.c
	@echo "--- broken (no fflush, exit() not _exit), exec fails, piped ---"
	-@./broken ./no-such-program | cat
	@echo "--- correct version, same conditions ---"
	-@$(MAKE) -s forkexec >/dev/null && ./forkexec ./no-such-program | cat
	@rm -f broken broken.c
	@rm -rf broken.dSYM

clean:
	rm -rf $(BINS) *.dSYM

.PHONY: all sanitize sanitize-addr run run-piped run-execfail demo-dup clean
