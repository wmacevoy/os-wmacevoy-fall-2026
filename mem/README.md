# mem

## bench

Times the same workload on the table and sequence structures of three languages:

| lang   | structures                                                                 | source      |
|--------|----------------------------------------------------------------------------|-------------|
| c++    | `std::map`, `std::unordered_map` (hash map), `std::vector`, `std::deque`   | `bench.cpp` |
| python | `dict`, `list`, `collections.deque`                                        | `bench.py`  |
| node   | plain object `{}`, `Map`, `Array` (no built-in deque)                      | `bench.js`  |

The keys are a shuffled permutation of `0..n-1`, shuffled by the same
xorshift32 generator in every language, so each structure sees the keys in the
same order. For each `n` it times the structure used as a table from key to
value:

- `insert`  — `table[key] = 2*key` for each key
- `lookup`  — sum `table[key]` for each key (all hits)
- `miss`    — test `n+key in table` for each key (all misses)
- `iterate` — sum every value
- `erase`   — remove each key

and, starting from empty, adding `n` entries at one end:

- `append-last`  — sequence: push at the back. map: keys `0, 1, ..., n-1`
- `append-first` — sequence: push at the front. map: keys `n-1, ..., 1, 0`

vector/list/deque/Array are used as tables indexed directly by key, so the
shuffled `insert` writes into slot `key` and shifts nothing; `append-first` is
where a sequence pays to shift. They grow on demand as keys arrive, and a slot
holding `EMPTY` (-1) has no key. Erase empties the slot and pops empty slots
off the end, so the memory really goes away (C++ vector calls `shrink_to_fit`
below a quarter full; the rest shrink on their own).

Each operation reports the best of `REPS` runs, in nanoseconds per element.
Every pass checks its answer, so the compiler can't throw the loops away.
Operations that cost O(n) per element are skipped (`-`) above n = 100,000,
where they would take minutes: `append-first` on vector/list/Array, and the
table ops on a Python deque, whose indexing walks blocks from the nearer end.

Three more columns show what memory did underneath:

- `resizes` — steps after which the container's own storage had moved:
  `data()` for a vector, `bucket_count()` for an unordered_map, `sys.getsizeof`
  for a dict or list. It is counted in a separate untimed run, since checking
  after every step costs time. A step that pops many empty slots can
  reallocate more than once but counts once. Maps (trees of nodes) and deques
  (lists of fixed-size blocks) have no single block to move: `-`. JavaScript
  can't see this at all: `-`.
- `allocs`, `frees` — calls to `operator new` / `operator delete` (C++ only;
  `bench.cpp` replaces them to count). A tree pays one per key; a hash map one
  per key plus one per rehash; a vector one per resize; a deque one per block.

```
make run                          # n = 1000 10000 100000 1000000, 3 reps
make run N="1000 1000000" REPS=5
./bench 100000                    # or run any one of them directly
./bench.py --reps 1 100000
./bench.js 100000
```

Things to look for:

- The tree (`std::map`) gets slower as `n` grows and falls out of cache; the
  hash map stays roughly flat; the vector is an order of magnitude faster than
  either because the key *is* the address.
- `append-last` vs `append-first`: vector/list/Array are ~2 ns vs ~6,500 ns
  per element at n = 100,000 (C++), because every push at the front moves
  everything over. A deque is cheap at both ends.
- A C++ deque indexes in O(1); a Python deque indexes in O(n).
- The hash tables (`std::unordered_map`, `dict`) grow by rehashing about
  log2(n) times but never shrink on erase. A vector doubles (~log2(n)
  reallocations); a Python list grows by ~1/8 (many more, each cheaper).
- In Python and node the interpreter/runtime overhead is most of the cost, so
  the gap between the structures shrinks.
- V8 stores integer keys of a plain object as array-like "elements", not as a
  hash of named properties, which is why ascending keys (`append-last`) are
  so much faster than descending ones.

## stats

`stats.cpp` is the small version of the trading example: polymorphic
statistics as **flyweights** over `std::vector`.

- A `Series` is one ticker's history: a symbol and a `std::vector<double>` of
  prices.
- A `Stat` knows *how* to compute one number from a history (`last`, `sma(w)`,
  `min(w)`, `max(w)`, `ewma(alpha)`, `vol(w)`) but holds no history. It is an
  abstract base class with a virtual `operator()`; each statistic is a
  subclass.
- Because a `Stat` is small and immutable, one instance is shared by every
  series: its intrinsic state (the window) lives in the object, the extrinsic
  state (the prices) is passed in. The `Flyweights` factory hands back the
  same object when asked for the same stat twice.

```
make stats && ./stats
```

Each stat recomputes from the vector every time it is asked, O(window). `hft`
is the fast version: it updates running sums on every tick instead.

## hft

`hft.cpp` is a "high speed trading" example: a simulated feed of trades
(ticker, price, size) for many tickers, and per-ticker statistics over each
ticker's last `W` trades, updated on every tick:

- `sma` — simple moving average of price
- `ewma` — exponentially weighted moving average, alpha = 2/(W+1)
- `vwap` — volume weighted average price
- `min`, `max` — lowest and highest price
- `vol` — volatility: standard deviation of log returns, annualized

Each ticker's price is a random walk (geometric Brownian motion) with its own
volatility `sigma`, so the measured `vol` should land near `sigma`. A few
tickers trade far more than the rest.

What makes it fast:

- every window is a fixed-size **ring buffer with running sums**: a tick adds
  the new sample and subtracts the one falling out, O(1)
- `min`/`max` use a **monotonic queue**: a new price throws out older prices
  it beats, since they can never be the answer again; O(1) amortized
- all memory is allocated before the feed starts, so the hot loop never calls
  `new` (the `allocs` column counts it, as in `bench.cpp`)

Finding each tick's ticker is timed three ways: `std::map` by symbol,
`std::unordered_map` by symbol, and a `std::vector` indexed by a numeric id,
which is what real feed handlers do (symbols become small integers once, up
front). At the end every ticker's stats are recomputed the slow way from its
whole history and compared, so the fast path is checked, not just timed.

```
make hft && ./hft
./hft --tickers 500 --ticks 2000000 --window 100 --seed 2026 --reps 3
```

Things to look for: vector[id] is about 4x faster than std::map and 2x
faster than std::unordered_map. `--window 1` is several times faster than
`--window 100`, likely because 500 tickers × ~5.6 KB of window each (~2.8 MB)
no longer fits in the faster caches.

## hft_omp

`hft_omp.cpp` is `hft` on several cores with OpenMP, always using the
vector[id] lookup. One ticker's ticks must be applied in order, so the
parallelism is across tickers, two ways:

- `shard` — every thread reads the whole feed and applies only the ticks of
  its own tickers (`id % threads`)
- `demux` — one serial counting sort groups the ticks by ticker, then
  `#pragma omp parallel for schedule(dynamic, 1)` hands whole tickers to
  threads

Results are bit-for-bit the serial ones and are checked against brute force
(that check is itself a `parallel for` with `reduction`, using `omp simd`
sums). `Stats` is `alignas(64)` to avoid false sharing between threads.

```
make hft_omp && ./hft_omp                 # macOS: needs brew install libomp
make omp-run                              # in a Linux container (Dockerfile)
make omp-run ARGS="--threads 4 --ticks 4000000"
OMP_NUM_THREADS=2 ./hft_omp
```

Things to look for: the `limit` column. One ticker trades ~15% of all ticks,
so no strategy can beat ~6.8x however many cores it gets, and `shard`'s fixed
`id % threads` split caps it lower still. `demux` pays a serial sort first
(Amdahl's law). Inside Docker Desktop, 8 threads can be slower than 4: the
VM's vCPUs share the host with everything else. The feed differs between
macOS and Linux because libc++ and libstdc++ implement the random
distributions differently, even with the same seed.
