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
(the floor, the door, the dock, stdout) are guarded by mutexes. A worker that
has to wait for a box, an empty bay or a full truck sleeps on a condition
variable until another worker notifies it, instead of polling.

```
./factory.py --forklifts 3 --trucks 3 --bays 2 --trips 2 --capacity 3
```

`factory.c` is the same program in C with pthreads (`pthread_cond_wait`,
`pthread_cond_signal`), and `factoryxx.cpp` in C++17, where the floor and the
dock are monitor classes built on `std::mutex`, `std::scoped_lock`,
`std::unique_lock` and `std::condition_variable`. Both take the same options.

```
make factory && ./factory --forklifts 3 --trucks 3 --bays 2 --trips 2 --capacity 3
make factoryxx && ./factoryxx --forklifts 3 --trucks 3 --bays 2 --trips 2 --capacity 3
```

`factoryxx_guarded.cpp` is the C++ factory again, built on `guarded.hpp`, a
small library of conditional critical regions. There are no condition
variables and no notify calls: each step names the resources it needs and the
condition it waits for, as in
`auto hold = when([&] { return !q.empty(); }, q);`. `when()` locks everything
in a fixed order, and while the condition is false it holds nothing and sleeps
until some other thread writes one of those resources. Accessors throw if the
calling thread doesn't hold the resource, so a forgotten lock fails loudly
instead of racing. `retry()` and `balk()` handle blocks that find out partway
through that they have to wait.

```
make factoryxx_guarded && ./factoryxx_guarded --forklifts 3 --trucks 3 --bays 2 --trips 2 --capacity 3
```
