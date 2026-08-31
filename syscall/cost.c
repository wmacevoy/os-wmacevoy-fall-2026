/* cost.c -- weigh the barrier in nanoseconds.
**
** Part 2 and part 3 of the demo. You cannot see the boundary; you can time it.
**
**   1. an ordinary function call        -- no barrier
**   2. getpid() via the raw instruction -- a real trap, every time
**   3. clock_gettime()                  -- LOOKS like a syscall. Time it anyway.
**
** Do not read ahead to the README before running it. The third number is the
** assignment.
*/
#include <stdio.h>
#include <time.h>
#include <stdint.h>

long raw_getpid(void);

/* Not static, not inlinable: we are timing a real call, not nothing. */
__attribute__((noinline)) long plain_call(long x) { return x + 1; }

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

#define N 300000

int main(void) {
    volatile long sink = 0;
    uint64_t t0, t1;

    t0 = now_ns();
    for (int i = 0; i < N; i++) sink += plain_call(i);
    t1 = now_ns();
    double fn = (double)(t1 - t0) / N;

    t0 = now_ns();
    for (int i = 0; i < N; i++) sink += raw_getpid();
    t1 = now_ns();
    double sc = (double)(t1 - t0) / N;

    t0 = now_ns();
    for (int i = 0; i < N; i++) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); sink += ts.tv_nsec; }
    t1 = now_ns();
    double cg = (double)(t1 - t0) / N;

    printf("  function call    %8.1f ns\n", fn);
    printf("  getpid()  [trap] %8.1f ns   %5.1fx the call\n", sc, sc / fn);
    printf("  clock_gettime()  %8.1f ns   %5.1fx the call\n", cg, cg / fn);
    printf("\n  Three numbers. Two of them make sense.\n");
    printf("  Why is the third one not %.0fx?\n", sc / fn);
    return (int)(sink & 0);
}
