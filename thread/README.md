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

## factory

`factory.py` simulates a factory with one real OS thread per worker: a machine
builds boxes, forklifts carry them through a one-lane dock door, and trucks
wait in loading bays until full. Each worker is a plain loop, and shared things
(the floor, the door, each bay, stdout) are guarded by mutexes.

```
./factory.py --forklifts 3 --trucks 3 --bays 2 --trips 2 --capacity 3
```
