# thread

## hello

`hello.c` starts a single worker thread with `pthread_create`, waits for it with
`pthread_join`, and prints what the worker handed back.

```
make hello && ./hello
```

## sum

C version of parallel add

```
make sum && /usr/bin/time ./sum --n $((1000*1000*1000) --threads 4
```

## sumxx

C++ version of parallel add, demonstrates templates & unroll

```
make sumxx && ./sum --n $((1000*1000*1000) --threads 4 --unroll
```
