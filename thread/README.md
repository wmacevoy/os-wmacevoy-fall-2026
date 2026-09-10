# thread — one worker, one return value

`hello.c` starts a single worker thread with `pthread_create`, waits for it with
`pthread_join`, and prints what the worker handed back.

```
make        # build
make run    # ./hello
make twice  # run it twice
make clean
```

Output:

```
main:                  thread 0x1eccadd80, process 54288
hello from the worker: thread 0x16d66f000, process 54288
main:                  the worker returned 42
```

Two thread ids, one process id. The worker is not a new process — it shares the
address space, the open files, and the pid with `main`; the only thing that is
its own is a stack (and the registers to go with it).

## The four calls

- `pthread_create(&worker_thread, NULL, worker, NULL)` — start `worker` on a new
  thread. `NULL` attributes means default stack size and a *joinable* thread.
  The last `NULL` is the argument passed to `worker`; here there is nothing to
  pass. It returns immediately, and now two threads are running.
- `pthread_self()` — this thread's own id, of opaque type `pthread_t`. It is a
  pointer on macOS and an integer on glibc, so the only portable thing to do
  with it is compare it (`pthread_equal`); printing it as a number is a debug
  convenience, not something to rely on.
- `return (void *)(intptr_t)42` — returning from the thread function ends the
  thread, and the value becomes its exit status. `pthread_exit((void *)(intptr_t)42)`
  from anywhere in the call stack does the same thing.
- `pthread_join(worker_thread, &result)` — block until that thread ends, then
  store its `void *` in `result` and release the thread's bookkeeping. A
  joinable thread that is never joined leaks that bookkeeping.

## Returning 42, and the cast

A thread returns a `void *`, so an `int` has to be smuggled through a pointer:
`(void *)(intptr_t)42` on the way out and `(int)(intptr_t)result` on the way
back. `intptr_t` (from `<stdint.h>`) is the integer type guaranteed to survive
the round trip through a pointer; casting `int` straight to `void *` warns,
because on a 64-bit machine the two are not the same size.

The alternative — and what you want as soon as the answer is bigger than a
number — is to return a pointer to real storage: `malloc` it in the worker and
`free` it in `main` after the join. What you must not return is the address of
one of the worker's locals. That memory is the worker's stack, and joining is
exactly the moment it goes away.

## Things to try

- Print the ids before and after the `printf`s in a loop and watch the two
  threads interleave — `printf` is thread-safe, but *which* line lands first is
  not decided by your code.
- Drop the `pthread_join`. `main` returns, and `exit` takes every thread with
  it: the worker's "hello" may never print at all.
- Start ten workers in an array of `pthread_t`, pass each its index (a pointer
  to its own `int`, not a pointer to the shared loop counter), and join them in
  order.
- Have the worker return a `malloc`'d struct instead of a smuggled `int`, and
  `free` it in `main` after the join.
- On Linux, add `#include <sys/syscall.h>` and print `syscall(SYS_gettid)` next
  to `getpid()` — the kernel's own thread id, and the number `ps -L` and `top -H`
  show. On macOS the equivalent is `pthread_threadid_np(NULL, &tid)`. Both are
  unportable, which is why `pthread_self` exists.
