//
// hft_omp.cpp -- hft.cpp, parallelized with OpenMP.
//
// Same simulated feed and the same per-ticker window stats as hft.cpp (sma,
// ewma, vwap, min, max, vol), always found by vector[id], the fastest lookup
// there. What changes is how many cores do the work.
//
// One ticker's ticks must be applied in order: every update reads the
// running sums the previous one left. So the hot loop cannot be split across
// ticks, and it cannot be vectorized (#pragma omp simd) either. But different
// tickers share nothing, so the parallelism is across tickers. Two ways:
//
//   shard : every thread reads the whole feed and applies only the ticks of
//           the tickers it owns (id % threads == thread). No preparation,
//           but every thread pays to read every tick.
//
//   demux : first one pass sorts the ticks by ticker (a counting sort into
//           one flat array, still in feed order within each ticker). Then
//           "#pragma omp parallel for" hands whole tickers to threads; each
//           thread reads only its own ticks, packed together. The sort is
//           serial, so it bounds the speedup (Amdahl's law).
//
// Either way, a ticker's ticks are applied in the same order by one thread,
// so the results are bit-for-bit those of the serial loop.
//
// Neither can beat its "limit" column: a few tickers trade far more than the
// rest, and a thread holding a busy ticker is the last to finish. limit is
// the speedup if time were just proportional to the busiest thread's ticks.
//
// Other OpenMP pieces used here:
//   - Stats is alignas(64), one cache line apart, so two threads updating
//     neighboring tickers do not fight over a line (false sharing)
//   - the brute-force check runs as a parallel for with reduction(+:bad), and
//     its sums use #pragma omp simd reductions (those loops have no
//     loop-carried dependence besides the reduction itself)
//   - the allocation counter is atomic now that several threads allocate
//
// build: clang++ -std=c++17 -O3 -fopenmp hft_omp.cpp -o hft_omp
//        (macOS: make hft_omp, which uses Homebrew's libomp)
//
// usage: ./hft_omp [--tickers N] [--ticks M] [--window W] [--seed S]
//                  [--reps R] [--threads T]
//

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <new>
#include <random>
#include <set>
#include <string>
#include <vector>

#include <omp.h>

// Every container allocates through these, so counting here sees it all.
// Atomic: the parallel check allocates from several threads at once.
static std::atomic<long> g_allocs{0};

void *operator new(std::size_t size) {
  ++g_allocs;
  if (void *p = std::malloc(size ? size : 1)) return p;
  throw std::bad_alloc();
}

// A vector of an alignas(64) type allocates through this one.
void *operator new(std::size_t size, std::align_val_t align) {
  ++g_allocs;
  std::size_t a = std::size_t(align);
  std::size_t n = (size + a - 1) / a * a; // aligned_alloc wants a multiple
  if (void *p = std::aligned_alloc(a, n ? n : a)) return p;
  throw std::bad_alloc();
}

void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete(void *p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void *p, std::size_t, std::align_val_t) noexcept {
  std::free(p);
}

// Simulated time: 10 trades a second per ticker, 6.5 hour days, 252 days a
// year. A per-trade standard deviation times sqrt(TICKS_PER_YEAR) is annual.
const double TICKS_PER_YEAR = 252 * 6.5 * 3600 * 10;

struct Tick {
  char symbol[12];    // "ABCD", zero padded -- what arrives on the wire
  std::uint32_t id;   // the same ticker as a dense index 0..N-1
  std::uint32_t size; // shares traded
  double price;
};

// What we simulate for each ticker.
struct Ticker {
  std::string symbol;
  double start;  // opening price
  double sigma;  // annual volatility, e.g. 0.30 is 30%
  double weight; // share of all trades
};

// A fixed-capacity FIFO. push() on a full ring overwrites the oldest entry and
// hands it back, so the caller can take it out of its running sums.
template <typename T> class Ring {
  std::vector<T> buf;
  std::size_t head = 0; // next slot to write
  std::size_t count = 0;

public:
  explicit Ring(std::size_t capacity) : buf(capacity) {}
  std::size_t size() const { return count; }
  bool full() const { return count == buf.size(); }
  // Returns true and sets *evicted if the ring was full.
  bool push(const T &x, T *evicted) {
    bool was_full = full();
    if (was_full) *evicted = buf[head];
    else ++count;
    buf[head] = x;
    head = (head + 1) % buf.size();
    return was_full;
  }
};

// Sliding-window min (Before = std::less) or max (std::greater).
// Holds (seq, price) with prices strictly "improving" from back to front:
// a new price throws out every older price it beats, since those can never
// be the answer again. The front is the answer; it leaves when it is older
// than the window. Each price is pushed and popped at most once: O(1)
// amortized. Stored in a ring of fixed capacity, so no allocation.
template <typename Before> class MonoQueue {
  struct Entry {
    std::uint64_t seq;
    double price;
  };
  std::vector<Entry> buf;
  std::size_t front_at = 0;
  std::size_t count = 0;
  std::uint64_t window;

  Entry &at(std::size_t i) { return buf[(front_at + i) % buf.size()]; }

public:
  explicit MonoQueue(std::size_t w) : buf(w), window(w) {}
  void push(std::uint64_t seq, double price) {
    while (count > 0 && at(0).seq + window <= seq) { // expired
      front_at = (front_at + 1) % buf.size();
      --count;
    }
    while (count > 0 && !Before()(at(count - 1).price, price)) --count;
    at(count++) = Entry{seq, price};
  }
  double front() { return at(0).price; }
};

struct Sample {
  double price;
  double ret; // log return from the previous trade
  double size;
};

// Everything we track for one ticker. alignas(64): each Stats starts on its
// own cache line, so the running sums of two tickers updated by two threads
// never share a line.
struct alignas(64) Stats {
  Ring<Sample> ring;
  MonoQueue<std::less<double>> lows;
  MonoQueue<std::greater<double>> highs;
  double sum_p = 0, sum_r = 0, sum_r2 = 0, sum_pv = 0, sum_v = 0;
  double alpha, ewma, last;
  std::uint64_t trades = 0;

  Stats(std::size_t window, double start)
      : ring(window), lows(window), highs(window),
        alpha(2.0 / (window + 1)), ewma(start), last(start) {}

  void add(const Sample &s, double sign) {
    sum_p += sign * s.price;
    sum_r += sign * s.ret;
    sum_r2 += sign * s.ret * s.ret;
    sum_pv += sign * s.price * s.size;
    sum_v += sign * s.size;
  }

  void on_trade(double price, std::uint32_t size) {
    Sample s{price, std::log(price / last), double(size)};
    Sample old;
    if (ring.push(s, &old)) add(old, -1);
    add(s, +1);
    lows.push(trades, price);
    highs.push(trades, price);
    ewma += alpha * (price - ewma);
    last = price;
    ++trades;
  }

  double n() const { return ring.size(); }
  double sma() const { return sum_p / n(); }
  double vwap() const { return sum_pv / sum_v; }
  double low() { return lows.front(); }
  double high() { return highs.front(); }
  double vol() const {
    if (n() < 2) return 0;
    double var = (sum_r2 - sum_r * sum_r / n()) / (n() - 1);
    return std::sqrt(std::max(var, 0.0) * TICKS_PER_YEAR);
  }
};

typedef std::vector<Stats> Index;

// ---- the simulated market -------------------------------------------------

std::vector<Ticker> make_tickers(int n, std::mt19937_64 &rng) {
  std::uniform_int_distribution<int> letter('A', 'Z');
  std::uniform_real_distribution<double> start(5, 500);
  std::uniform_real_distribution<double> sigma(0.10, 0.80);
  std::set<std::string> taken;
  std::vector<Ticker> tickers;
  for (int i = 0; i < n; ++i) {
    std::string sym;
    do {
      sym.clear();
      int len = 3 + (i % 2);
      for (int k = 0; k < len; ++k) sym += char(letter(rng));
    } while (!taken.insert(sym).second);
    // Zipf-like activity: a few names trade far more than the rest.
    tickers.push_back({sym, start(rng), sigma(rng), 1.0 / (i + 1)});
  }
  return tickers;
}

// Each trade picks a ticker by weight and moves its price by a geometric
// Brownian motion step: price *= exp(sigma*sqrt(dt)*Z - sigma^2*dt/2).
// Serial on purpose: one random stream, so the feed matches hft.cpp's.
std::vector<Tick> make_feed(const std::vector<Ticker> &tickers, long count,
                            std::mt19937_64 &rng) {
  std::vector<double> weights, price, step;
  for (const Ticker &t : tickers) {
    weights.push_back(t.weight);
    price.push_back(t.start);
    step.push_back(t.sigma / std::sqrt(TICKS_PER_YEAR));
  }
  std::discrete_distribution<std::uint32_t> pick(weights.begin(),
                                                 weights.end());
  std::normal_distribution<double> z(0, 1);
  std::uniform_int_distribution<std::uint32_t> lots(1, 10);

  std::vector<Tick> feed(count);
  for (Tick &t : feed) {
    std::uint32_t id = pick(rng);
    double s = step[id];
    price[id] *= std::exp(s * z(rng) - s * s / 2);
    std::memset(t.symbol, 0, sizeof t.symbol);
    std::memcpy(t.symbol, tickers[id].symbol.data(), tickers[id].symbol.size());
    t.id = id;
    t.size = 100 * lots(rng);
    t.price = price[id];
  }
  return feed;
}

// ---- serial, shard and demux ------------------------------------------------

struct Result {
  double sec;
  long allocs;
};

typedef std::chrono::steady_clock Clock;

double seconds(Clock::time_point t0, Clock::time_point t1) {
  return std::chrono::duration<double>(t1 - t0).count();
}

// hft.cpp's vector[id] loop, untouched.
Result run_serial(const std::vector<Tick> &feed, Index &index) {
  long a0 = g_allocs;
  auto t0 = Clock::now();
  for (const Tick &t : feed) index[t.id].on_trade(t.price, t.size);
  auto t1 = Clock::now();
  return {seconds(t0, t1), g_allocs - a0};
}

// Every thread scans the whole feed and keeps the ticks of its own tickers.
Result run_shard(const std::vector<Tick> &feed, Index &index, int threads) {
  long a0 = g_allocs;
  auto t0 = Clock::now();
#pragma omp parallel num_threads(threads)
  {
    std::uint32_t me = omp_get_thread_num(), n = omp_get_num_threads();
    for (const Tick &t : feed)
      if (t.id % n == me) index[t.id].on_trade(t.price, t.size);
  }
  auto t1 = Clock::now();
  return {seconds(t0, t1), g_allocs - a0};
}

// What the parallel phase of demux reads: just the fields on_trade needs.
struct Trade {
  double price;
  std::uint32_t size;
};

// The feed sorted by ticker: ticker k's trades are trades[start[k]] up to
// trades[start[k+1]], in feed order. Allocated once, before any clock runs.
struct Demux {
  std::vector<std::size_t> start, next;
  std::vector<Trade> trades;
  Demux(std::size_t ntickers, std::size_t nticks)
      : start(ntickers + 1), next(ntickers), trades(nticks) {}
};

// Counting sort the feed by ticker (serial), then whole tickers to threads.
// The busiest tickers have the lowest ids, so schedule(dynamic, 1) in id
// order hands out the big jobs first and fills in with small ones.
Result run_demux(const std::vector<Tick> &feed, Index &index, int threads,
                 Demux &d, double *sort_sec) {
  long a0 = g_allocs;
  auto t0 = Clock::now();
  std::fill(d.start.begin(), d.start.end(), 0);
  for (const Tick &t : feed) ++d.start[t.id + 1];
  for (std::size_t k = 1; k < d.start.size(); ++k) d.start[k] += d.start[k - 1];
  std::copy(d.start.begin(), d.start.end() - 1, d.next.begin());
  for (const Tick &t : feed) d.trades[d.next[t.id]++] = {t.price, t.size};
  auto t1 = Clock::now();

  long ntickers = long(index.size());
#pragma omp parallel for num_threads(threads) schedule(dynamic, 1)
  for (long k = 0; k < ntickers; ++k)
    for (std::size_t j = d.start[k]; j < d.start[k + 1]; ++j)
      index[k].on_trade(d.trades[j].price, d.trades[j].size);
  auto t2 = Clock::now();

  *sort_sec = seconds(t0, t1);
  return {seconds(t0, t2), g_allocs - a0};
}

// ---- how well the work can be balanced --------------------------------------

// Speedup if time were proportional to the busiest thread's ticks.
double shard_limit(const std::vector<long> &count, int threads) {
  std::vector<long> load(threads);
  long total = 0;
  for (std::size_t k = 0; k < count.size(); ++k) {
    load[k % threads] += count[k];
    total += count[k];
  }
  return double(total) / *std::max_element(load.begin(), load.end());
}

// Same for demux: dynamic scheduling in id order gives each ticker to the
// thread that frees up first, i.e. the least loaded one.
double demux_limit(const std::vector<long> &count, int threads) {
  std::vector<long> load(threads);
  long total = 0;
  for (long c : count) {
    *std::min_element(load.begin(), load.end()) += c;
    total += c;
  }
  return double(total) / *std::max_element(load.begin(), load.end());
}

// ---- checking the fast path against brute force ---------------------------

bool close(double a, double b) {
  return std::fabs(a - b) <= 1e-9 * std::max(1.0, std::fabs(b));
}

// One ticker's whole history: its opening price, then every trade.
struct History {
  std::vector<double> prices;
  std::vector<double> sizes;
};

std::vector<History> histories(const std::vector<Ticker> &tickers,
                               const std::vector<Tick> &feed) {
  std::vector<History> h(tickers.size());
  for (std::size_t i = 0; i < tickers.size(); ++i) {
    h[i].prices.push_back(tickers[i].start);
    h[i].sizes.push_back(0);
  }
  for (const Tick &t : feed) {
    h[t.id].prices.push_back(t.price);
    h[t.id].sizes.push_back(t.size);
  }
  return h;
}

// Recompute one ticker's window stats from scratch and compare.
bool check(Stats &s, const History &h, std::size_t window) {
  const double *prices = h.prices.data(), *sizes = h.sizes.data();
  std::size_t trades = h.prices.size() - 1;
  if (trades != s.trades) return false;
  if (trades == 0) return true;
  std::size_t n = std::min(window, trades);
  std::size_t first = h.prices.size() - n;

  // Independent iterations combined by +: SIMD lanes each keep a partial
  // sum and they are added up at the end.
  double sum_p = 0, sum_pv = 0, sum_v = 0, low = 1e300, high = -1e300;
#pragma omp simd reduction(+ : sum_p, sum_pv, sum_v)
  for (std::size_t i = first; i < first + n; ++i) {
    sum_p += prices[i];
    sum_pv += prices[i] * sizes[i];
    sum_v += sizes[i];
  }
  // reduction(min/max) is legal OpenMP too, but clang will not vectorize a
  // floating-point std::min without being promised there are no NaNs
  // (-ffinite-math-only), and says so with a warning. Plain loop instead.
  for (std::size_t i = first; i < first + n; ++i) {
    low = std::min(low, prices[i]);
    high = std::max(high, prices[i]);
  }

  std::vector<double> rets(n);
  for (std::size_t i = 0; i < n; ++i)
    rets[i] = std::log(prices[first + i] / prices[first + i - 1]);
  double mean = 0, var = 0;
#pragma omp simd reduction(+ : mean)
  for (std::size_t i = 0; i < n; ++i) mean += rets[i] / n;
#pragma omp simd reduction(+ : var)
  for (std::size_t i = 0; i < n; ++i) var += (rets[i] - mean) * (rets[i] - mean);
  double vol = n < 2 ? 0 : std::sqrt(var / (n - 1) * TICKS_PER_YEAR);

  // Each step needs the previous ewma: a true dependence, so no simd here.
  double alpha = 2.0 / (window + 1), ewma = prices[0];
  for (std::size_t i = 1; i <= trades; ++i) ewma += alpha * (prices[i] - ewma);

  // Running sums drift a little from cancellation; volatility the most,
  // since it subtracts two nearly equal sums of tiny squared returns.
  return close(s.sma(), sum_p / n) && close(s.vwap(), sum_pv / sum_v) &&
         s.low() == low && s.high() == high && close(s.ewma, ewma) &&
         std::fabs(s.vol() - vol) <= 1e-6 * std::max(1.0, vol);
}

// Check every ticker of every index; returns the number that disagree.
int check_all(std::vector<Index *> &indexes, const std::vector<History> &hist,
              std::size_t window, int threads) {
  int bad = 0;
  long ntickers = long(hist.size());
#pragma omp parallel for num_threads(threads) schedule(dynamic) \
    reduction(+ : bad)
  for (long i = 0; i < ntickers; ++i)
    for (Index *index : indexes) bad += !check((*index)[i], hist[i], window);
  return bad;
}

// ---- main -----------------------------------------------------------------

int main(int argc, const char *argv[]) {
  int ntickers = 500;
  long nticks = 2'000'000;
  std::size_t window = 100;
  unsigned long seed = 2026;
  int reps = 3;
  int max_threads = omp_get_max_threads(); // OMP_NUM_THREADS, else the cores
  for (int i = 1; i + 1 < argc; i += 2) {
    const char *opt = argv[i], *val = argv[i + 1];
    if (!std::strcmp(opt, "--tickers")) ntickers = std::atoi(val);
    else if (!std::strcmp(opt, "--ticks")) nticks = std::atol(val);
    else if (!std::strcmp(opt, "--window")) window = std::atol(val);
    else if (!std::strcmp(opt, "--seed")) seed = std::strtoul(val, 0, 10);
    else if (!std::strcmp(opt, "--reps")) reps = std::atoi(val);
    else if (!std::strcmp(opt, "--threads")) max_threads = std::atoi(val);
    else {
      std::fprintf(stderr, "usage: %s [--tickers N] [--ticks M] [--window W]"
                           " [--seed S] [--reps R] [--threads T]\n", argv[0]);
      return 1;
    }
  }
  if (ntickers < 1 || nticks < 1 || window < 1 || reps < 1 ||
      max_threads < 1) {
    std::fprintf(stderr,
                 "tickers, ticks, window, reps and threads must be positive\n");
    return 1;
  }

  std::mt19937_64 rng(seed);
  std::vector<Ticker> tickers = make_tickers(ntickers, rng);
  std::vector<Tick> feed = make_feed(tickers, nticks, rng);
  std::printf("%d tickers, %ld ticks, window %zu, best of %d, "
              "up to %d threads (%d cores)\n\n",
              ntickers, nticks, window, reps, max_threads, omp_get_num_procs());

  std::vector<long> count(ntickers);
  for (const Tick &t : feed) ++count[t.id];

  auto fresh = [&] {
    Index v;
    v.reserve(tickers.size());
    for (const Ticker &t : tickers) v.emplace_back(window, t.start);
    return v;
  };
  Demux demux(ntickers, nticks);

  // Start the thread pool now, so no timed run pays to create threads.
#pragma omp parallel num_threads(max_threads)
  {}

  // 1, 2, 4, ... threads, and the maximum.
  std::vector<int> sweep;
  for (int t = 1; t < max_threads; t *= 2) sweep.push_back(t);
  sweep.push_back(max_threads);

  std::printf("%-8s %7s %9s %10s %8s %6s %7s\n", "strategy", "threads",
              "ns/tick", "Mticks/s", "speedup", "limit", "allocs");
  auto row = [&](const char *name, int threads, Result r, double base,
                 double limit) {
    std::printf("%-8s %7d %9.1f %10.1f %7.2fx %5.2fx %7ld\n", name, threads,
                1e9 * r.sec / nticks, nticks / r.sec / 1e6, base / r.sec,
                limit, r.allocs);
  };
  // Best of reps, each rep on fresh Stats built before its clock starts.
  auto best = [&](Index &index, std::function<Result()> go) {
    Result b{1e300, 0};
    for (int r = 0; r < reps; ++r) {
      index = fresh();
      Result res = go();
      if (res.sec < b.sec) b = res;
    }
    return b;
  };

  Index serial, shard, sorted;
  Result base = best(serial, [&] { return run_serial(feed, serial); });
  row("serial", 1, base, base.sec, 1.0);

  for (int t : sweep) {
    Result r = best(shard, [&] { return run_shard(feed, shard, t); });
    row("shard", t, r, base.sec, shard_limit(count, t));
  }
  double sort_sec = 1e300;
  for (int t : sweep) {
    Result r = best(sorted, [&] {
      double s;
      Result res = run_demux(feed, sorted, t, demux, &s);
      sort_sec = std::min(sort_sec, s);
      return res;
    });
    row("demux", t, r, base.sec, demux_limit(count, t));
  }
  std::printf("\ndemux's serial counting sort alone: %.1f ns/tick (%.0f%% of "
              "the serial loop)\n",
              1e9 * sort_sec / nticks, 100 * sort_sec / base.sec);

  // The busiest tickers, from the last demux run (all three agree; checked).
  std::printf("\n%-6s %8s %9s %9s %9s %9s %9s %9s %7s %7s\n", "symbol",
              "trades", "last", "sma", "ewma", "vwap", "min", "max", "vol",
              "sigma");
  for (int i = 0; i < std::min(ntickers, 10); ++i) {
    Stats &s = sorted[i];
    std::printf("%-6s %8llu %9.2f %9.2f %9.2f %9.2f %9.2f %9.2f %6.1f%% "
                "%6.1f%%\n",
                tickers[i].symbol.c_str(), (unsigned long long)s.trades, s.last,
                s.sma(), s.ewma, s.vwap(), s.low(), s.high(), 100 * s.vol(),
                100 * tickers[i].sigma);
  }

  std::vector<History> hist = histories(tickers, feed);
  std::vector<Index *> all = {&serial, &shard, &sorted};
  auto c0 = Clock::now();
  int bad1 = check_all(all, hist, window, 1);
  auto c1 = Clock::now();
  int bad = check_all(all, hist, window, max_threads);
  auto c2 = Clock::now();
  bad += bad1;
  std::printf("\ncheck: %s (%d tickers x 3 strategies against brute force; "
              "%.1f ms on 1 thread, %.1f ms on %d)\n",
              bad ? "FAILED" : "ok", ntickers, 1e3 * seconds(c0, c1),
              1e3 * seconds(c1, c2), max_threads);
  return bad ? 1 : 0;
}
