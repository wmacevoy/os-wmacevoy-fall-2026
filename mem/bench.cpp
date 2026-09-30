//
// bench.cpp -- time table and sequence operations on std::map,
// std::unordered_map (the standard hash_map), std::vector and std::deque.
//
// Same workload as bench.py and bench.js, so the numbers line up.
// The first five ops use the structure as a table from key to value:
//
//   keys   = a shuffled permutation of 0..n-1 (same order in every language)
//   insert : table[key] = 2*key            for each key
//   lookup : sum += table[key]             for each key   (all hits)
//   miss   : count += (n+key in table)     for each key   (all misses)
//   iterate: sum += value                  for every entry
//   erase  : remove key                    for each key
//
// The last two start from empty and add n entries, each one at an end:
//
//   append-last : sequence: push at the back.  map: keys 0, 1, ..., n-1
//   append-first: sequence: push at the front. map: keys n-1, ..., 1, 0
//
// Besides time, each op reports how memory moved underneath it:
//
//   resizes: steps after which the container's own storage had moved
//            (vector: data() moved, unordered_map: bucket_count changed;
//            "-" for map and deque, which have no single block to move)
//   allocs : calls to operator new    (counted by replacing it below)
//   frees  : calls to operator delete
//
// usage: ./bench [--reps R] [n ...]
//

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <new>
#include <type_traits>
#include <unordered_map>
#include <vector>
#include <functional>
#include <algorithm>

typedef std::int64_t Key;
typedef std::int64_t Value;

// Every container allocates through these, so counting here sees it all.
static long g_allocs = 0;
static long g_frees = 0;

void *operator new(std::size_t size) {
  ++g_allocs;
  if (void *p = std::malloc(size ? size : 1)) return p;
  throw std::bad_alloc();
}

void operator delete(void *p) noexcept {
  if (p) ++g_frees;
  std::free(p);
}

void operator delete(void *p, std::size_t) noexcept { operator delete(p); }

// xorshift32 -- tiny RNG that is easy to write identically in C++, Python and JS.
struct Rng {
  std::uint32_t x;
  Rng(std::uint32_t seed) : x(seed) {}
  std::uint32_t next() {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
  }
};

// Fisher-Yates shuffle of 0..n-1
std::vector<Key> shuffled(Key n) {
  std::vector<Key> keys(n);
  for (Key i = 0; i < n; ++i) keys[i] = i;
  Rng rng(2026);
  for (Key i = n - 1; i > 0; --i) {
    Key j = rng.next() % (i + 1);
    std::swap(keys[i], keys[j]);
  }
  return keys;
}

void check(bool ok, const char *what) {
  if (!ok) {
    std::fprintf(stderr, "check failed: %s\n", what);
    std::exit(1);
  }
}

const char *OPS[] = {"insert", "lookup",      "miss",        "iterate",
                     "erase",  "append-last", "append-first"};
const int NOPS = 7;

// Ops that cost O(n) per step (so n^2 in all) are skipped above this size.
// Here that is vector append-first: every push shifts the whole vector over.
const Key QUADRATIC_MAX = 100'000;

struct Stat {
  double sec = 0;
  long resizes = 0;
  long allocs = 0;
  long frees = 0;
  bool skipped = false;
};

// Time one call of f and count the heap traffic it caused.
Stat measure(const std::function<void()> &f) {
  Stat s;
  long a0 = g_allocs, f0 = g_frees;
  auto t0 = std::chrono::steady_clock::now();
  f();
  auto t1 = std::chrono::steady_clock::now();
  s.sec = std::chrono::duration<double>(t1 - t0).count();
  s.allocs = g_allocs - a0;
  s.frees = g_frees - f0;
  return s;
}

Stat skipped() {
  Stat s;
  s.skipped = true;
  return s;
}

// What changes when a container reallocates its own storage.
// std::map is a tree of separately allocated nodes and std::deque is a list
// of fixed-size blocks: neither has one block that moves, so neither is known.
template <typename T> struct Footprint {
  static const bool known = false;
  static std::uintptr_t of(const T &) { return 0; }
};
template <> struct Footprint<std::unordered_map<Key, Value>> {
  static const bool known = true;
  static std::uintptr_t of(const std::unordered_map<Key, Value> &m) {
    return m.bucket_count();
  }
};
template <> struct Footprint<std::vector<Value>> {
  static const bool known = true;
  static std::uintptr_t of(const std::vector<Value> &v) {
    return reinterpret_cast<std::uintptr_t>(v.data());
  }
};

// Checking the footprint after every step costs time, so the passes are
// templates: the timed runs use NoProbe (compiles to nothing), and one
// separate untimed run uses ResizeProbe to count.
struct NoProbe {
  template <typename T> void start(const T &) {}
  template <typename T> void operator()(const T &) {}
  long count = 0;
};

struct ResizeProbe {
  std::uintptr_t last = 0;
  long count = 0;
  template <typename T> void start(const T &t) {
    last = Footprint<T>::of(t);
    count = 0;
  }
  template <typename T> void operator()(const T &t) {
    std::uintptr_t now = Footprint<T>::of(t);
    if (now != last) ++count;
    last = now;
  }
};

// Add entries 0..n-1 at one end. On a map "the end" is the key order.
template <typename Map> void append_last(Map &m, Key, Key i) { m[i] = 2 * i; }
template <typename Map> void append_first(Map &m, Key n, Key i) {
  Key k = n - 1 - i;
  m[k] = 2 * k;
}
void append_last(std::vector<Value> &v, Key, Key i) { v.push_back(2 * i); }
void append_first(std::vector<Value> &v, Key n, Key i) {
  v.insert(v.begin(), 2 * (n - 1 - i));
}
void append_last(std::deque<Value> &d, Key, Key i) { d.push_back(2 * i); }
void append_first(std::deque<Value> &d, Key n, Key i) {
  d.push_front(2 * (n - 1 - i));
}

// Start empty, then step(table, n, i) for i = 0..n-1.
template <typename Table, typename Probe, typename Step>
Stat appending(Key n, Probe &probe, Step step) {
  Table table;
  probe.start(table);
  Stat s = measure([&] {
    for (Key i = 0; i < n; ++i) {
      step(table, n, i);
      probe(table);
    }
  });
  s.resizes = probe.count;
  check(Key(table.size()) == n, "append size");
  return s;
}

// One pass over all ops for an associative container
// (std::map or std::unordered_map -- they share an interface).
template <typename Table, typename Probe>
void assoc(const std::vector<Key> &keys, Stat s[NOPS], Probe &probe) {
  Key n = keys.size();
  Table table;
  Value sum = 0, count = 0;

  probe.start(table);
  s[0] = measure([&] {
    for (Key k : keys) {
      table[k] = 2 * k;
      probe(table);
    }
  });
  s[0].resizes = probe.count;

  s[1] = measure([&] { for (Key k : keys) sum += table.find(k)->second; });
  check(sum == n * (n - 1), "lookup sum");

  s[2] = measure([&] { for (Key k : keys) count += table.count(n + k); });
  check(count == 0, "miss count");

  sum = 0;
  s[3] = measure([&] { for (auto &kv : table) sum += kv.second; });
  check(sum == n * (n - 1), "iterate sum");

  probe.start(table);
  s[4] = measure([&] {
    for (Key k : keys) {
      table.erase(k);
      probe(table);
    }
  });
  s[4].resizes = probe.count;
  check(table.empty(), "erase empty");

  s[5] = appending<Table>(
      n, probe, [](Table &t, Key n, Key i) { append_last(t, n, i); });
  s[6] = appending<Table>(
      n, probe, [](Table &t, Key n, Key i) { append_first(t, n, i); });
}

// A vector or deque is a "table" whose keys are exactly 0..n-1: the key *is*
// the index. It grows on demand as keys arrive, and a slot holding EMPTY has
// no key. Erase empties the slot and pops empty slots off the end. A vector
// gives memory back once under a quarter full; a deque frees its blocks as
// they empty on its own.
const Value EMPTY = -1;

template <typename Seq, typename Probe>
void seq(const std::vector<Key> &keys, Stat s[NOPS], Probe &probe) {
  const bool is_vector = std::is_same<Seq, std::vector<Value>>::value;
  Key n = keys.size();
  Seq table;
  Value sum = 0, count = 0;

  probe.start(table);
  s[0] = measure([&] {
    for (Key k : keys) {
      if (k >= Key(table.size())) table.resize(k + 1, EMPTY);
      table[k] = 2 * k;
      probe(table);
    }
  });
  s[0].resizes = probe.count;

  s[1] = measure([&] { for (Key k : keys) sum += table[k]; });
  check(sum == n * (n - 1), "lookup sum");

  s[2] = measure([&] {
    Key size = table.size();
    for (Key k : keys) count += (n + k < size && table[n + k] != EMPTY);
  });
  check(count == 0, "miss count");

  sum = 0;
  s[3] = measure([&] {
    for (Value v : table)
      if (v != EMPTY) sum += v;
  });
  check(sum == n * (n - 1), "iterate sum");

  probe.start(table);
  s[4] = measure([&] {
    for (Key k : keys) {
      table[k] = EMPTY;
      while (!table.empty() && table.back() == EMPTY) table.pop_back();
      if constexpr (is_vector) {
        if (table.size() < table.capacity() / 4) table.shrink_to_fit();
      }
      probe(table);
    }
  });
  s[4].resizes = probe.count;
  check(table.empty(), "erase empty");

  s[5] = appending<Seq>(
      n, probe, [](Seq &t, Key n, Key i) { append_last(t, n, i); });
  if (is_vector && n > QUADRATIC_MAX) {
    s[6] = skipped();
  } else {
    s[6] = appending<Seq>(
        n, probe, [](Seq &t, Key n, Key i) { append_first(t, n, i); });
  }
}

// pass(stats, probe) runs all ops once on one structure.
template <typename Pass>
void report(const char *name, Key n, int reps, bool resizable,
            const Pass &pass) {
  // One untimed run to count resizes, then the timed runs.
  Stat counted[NOPS];
  ResizeProbe rp;
  pass(counted, rp);

  Stat best[NOPS];
  for (int op = 0; op < NOPS; ++op) best[op].sec = 1e300;
  for (int r = 0; r < reps; ++r) {
    Stat s[NOPS];
    NoProbe np;
    pass(s, np);
    for (int op = 0; op < NOPS; ++op) {
      if (s[op].skipped || s[op].sec < best[op].sec) best[op] = s[op];
    }
  }
  for (int op = 0; op < NOPS; ++op) {
    const Stat &b = best[op];
    char ns[32], resizes[32], allocs[32], frees[32];
    if (b.skipped) {
      std::strcpy(ns, "-");
      std::strcpy(allocs, "-");
      std::strcpy(frees, "-");
    } else {
      std::snprintf(ns, sizeof ns, "%.1f", 1e9 * b.sec / n);
      std::snprintf(allocs, sizeof allocs, "%ld", b.allocs);
      std::snprintf(frees, sizeof frees, "%ld", b.frees);
    }
    if (b.skipped || !resizable) {
      std::strcpy(resizes, "-");
    } else {
      std::snprintf(resizes, sizeof resizes, "%ld", counted[op].resizes);
    }
    std::printf("%-8s %-22s %10lld %-12s %10s %8s %8s %8s\n", "c++", name,
                (long long)n, OPS[op], ns, resizes, allocs, frees);
  }
  std::fflush(stdout);
}

int main(int argc, const char *argv[]) {
  int reps = 3;
  std::vector<Key> sizes;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--reps") == 0 && i + 1 < argc) {
      reps = std::atoi(argv[++i]);
    } else {
      sizes.push_back(std::atoll(argv[i]));
    }
  }
  if (sizes.empty()) sizes = {1'000, 10'000, 100'000, 1'000'000};

  std::printf("%-8s %-22s %10s %-12s %10s %8s %8s %8s\n", "lang", "structure",
              "n", "op", "ns/op", "resizes", "allocs", "frees");
  for (Key n : sizes) {
    std::vector<Key> keys = shuffled(n);
    typedef std::map<Key, Value> Map;
    typedef std::unordered_map<Key, Value> Hash;
    typedef std::vector<Value> Vector;
    typedef std::deque<Value> Deque;
    report("std::map", n, reps, Footprint<Map>::known,
           [&](Stat s[NOPS], auto &probe) { assoc<Map>(keys, s, probe); });
    report("std::unordered_map", n, reps, Footprint<Hash>::known,
           [&](Stat s[NOPS], auto &probe) { assoc<Hash>(keys, s, probe); });
    report("std::vector", n, reps, Footprint<Vector>::known,
           [&](Stat s[NOPS], auto &probe) { seq<Vector>(keys, s, probe); });
    report("std::deque", n, reps, Footprint<Deque>::known,
           [&](Stat s[NOPS], auto &probe) { seq<Deque>(keys, s, probe); });
  }
  return 0;
}
