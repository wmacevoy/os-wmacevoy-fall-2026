 \//
// hft.cpp -- "high speed trading" stats on a simulated tick stream.
//
// A feed of trades (ticker, price, size) arrives one tick at a time. For
// every ticker we keep, over its last W trades:
//
//   sma   : simple moving average of price
//   ewma  : exponentially weighted moving average, alpha = 2/(W+1)
//   vwap  : volume weighted average price, sum(price*size)/sum(size)
//   min   : lowest price
//   max   : highest price
//   vol   : volatility, the standard deviation of log returns,
//           annualized so it can be compared to the volatility we simulated
//
// The point is how to do that fast:
//
//   - every window is a fixed-size ring buffer with running sums, so a tick
//     is O(1): add the new sample, subtract the one falling out
//   - min/max use a monotonic queue, O(1) amortized per tick
//   - all memory is allocated before the feed starts; the hot loop never
//     calls new (counted below by replacing operator new, as in bench.cpp)
//   - finding a tick's ticker is timed three ways: std::map by symbol,
//     std::unordered_map by symbol, and a vector indexed by a numeric id
//     (what real feed handlers do: symbols become small integers up front)
//
// Afterwards every ticker's stats are recomputed the slow way from the whole
// stream and compared, so the fast path is checked, not just timed.
//
// usage: ./hft [--tickers N] [--ticks M] [--window W] [--seed S] [--reps R]
//

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <new>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

// Every container allocates through these, so counting here sees it all.
static long g_allocs = 0;

void *operator new(std::size_t size) {
  ++g_allocs;
  if (void *p = std::malloc(size ? size : 1)) return p;
  throw std::bad_alloc();
}

void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }

// Simulated time: 10 trades a second per ticker, 6.5 hour days, 252 days a
// year. A per-trade standard deviation times sqrt(TICKS_PER_YEAR) is annual.
const double TICKS_PER_YEAR = 252 * 6.5 * 3600 * 10;

struct Tick {
  char symbol[8];     // "ABCD", zero padded -- what arrives on the wire
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

// Everything we track for one ticker.
struct Stats {
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

// ---- the three ways to find a tick's ticker --------------------------------

struct Result {
  double sec;
  long allocs;
};

// Run the whole feed through on_trade, finding each tick's Stats with find.
template <typename Find> Result run(const std::vector<Tick> &feed, Find find) {
  long a0 = g_allocs;
  auto t0 = std::chrono::steady_clock::now();
  for (const Tick &t : feed) find(t).on_trade(t.price, t.size);
  auto t1 = std::chrono::steady_clock::now();
  return {std::chrono::duration<double>(t1 - t0).count(), g_allocs - a0};
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
  const std::vector<double> &prices = h.prices, &sizes = h.sizes;
  std::size_t trades = prices.size() - 1;
  if (trades != s.trades) return false;
  if (trades == 0) return true;
  std::size_t n = std::min(window, trades);
  std::size_t first = prices.size() - n;

  double sum_p = 0, sum_pv = 0, sum_v = 0, low = 1e300, high = -1e300;
  std::vector<double> rets;
  for (std::size_t i = first; i < prices.size(); ++i) {
    sum_p += prices[i];
    sum_pv += prices[i] * sizes[i];
    sum_v += sizes[i];
    low = std::min(low, prices[i]);
    high = std::max(high, prices[i]);
    rets.push_back(std::log(prices[i] / prices[i - 1]));
  }
  double mean = 0, var = 0;
  for (double r : rets) mean += r / n;
  for (double r : rets) var += (r - mean) * (r - mean);
  double vol = n < 2 ? 0 : std::sqrt(var / (n - 1) * TICKS_PER_YEAR);

  double alpha = 2.0 / (window + 1), ewma = prices[0];
  for (std::size_t i = 1; i < prices.size(); ++i)
    ewma += alpha * (prices[i] - ewma);

  // Running sums drift a little from cancellation; volatility the most,
  // since it subtracts two nearly equal sums of tiny squared returns.
  return close(s.sma(), sum_p / n) && close(s.vwap(), sum_pv / sum_v) &&
         s.low() == low && s.high() == high && close(s.ewma, ewma) &&
         std::fabs(s.vol() - vol) <= 1e-6 * std::max(1.0, vol);
}

// ---- main -----------------------------------------------------------------

int main(int argc, const char *argv[]) {
  int ntickers = 500;
  long nticks = 2'000'000;
  std::size_t window = 100;
  unsigned long seed = 2026;
  int reps = 3;
  for (int i = 1; i + 1 < argc; i += 2) {
    const char *opt = argv[i], *val = argv[i + 1];
    if (!std::strcmp(opt, "--tickers")) ntickers = std::atoi(val);
    else if (!std::strcmp(opt, "--ticks")) nticks = std::atol(val);
    else if (!std::strcmp(opt, "--window")) window = std::atol(val);
    else if (!std::strcmp(opt, "--seed")) seed = std::strtoul(val, 0, 10);
    else if (!std::strcmp(opt, "--reps")) reps = std::atoi(val);
    else {
      std::fprintf(stderr, "usage: %s [--tickers N] [--ticks M] [--window W]"
                           " [--seed S] [--reps R]\n", argv[0]);
      return 1;
    }
  }
  if (ntickers < 1 || nticks < 1 || window < 1 || reps < 1) {
    std::fprintf(stderr, "tickers, ticks, window and reps must be positive\n");
    return 1;
  }

  std::mt19937_64 rng(seed);
  std::vector<Ticker> tickers = make_tickers(ntickers, rng);
  std::vector<Tick> feed = make_feed(tickers, nticks, rng);
  std::printf("%d tickers, %ld ticks, window %zu, best of %d\n\n", ntickers,
              nticks, window, reps);

  // All three lookups own their own Stats, built before the clock starts.
  typedef std::map<std::string, Stats> Tree;
  typedef std::unordered_map<std::string, Stats> Hash;
  typedef std::vector<Stats> Index;
  auto fresh_tree = [&] {
    Tree m;
    for (const Ticker &t : tickers) m.emplace(t.symbol, Stats(window, t.start));
    return m;
  };
  auto fresh_hash = [&] {
    Hash m;
    m.reserve(tickers.size());
    for (const Ticker &t : tickers) m.emplace(t.symbol, Stats(window, t.start));
    return m;
  };
  auto fresh_index = [&] {
    Index v;
    v.reserve(tickers.size());
    for (const Ticker &t : tickers) v.emplace_back(window, t.start);
    return v;
  };

  // The symbol is short enough for std::string's small-string buffer, so
  // building the key does not allocate: the allocs column should read 0.
  Tree tree;
  Hash hash;
  Index index;
  std::function<Result()> ways[] = {
      [&] {
        tree = fresh_tree();
        return run(feed, [&](const Tick &t) -> Stats & {
          return tree.find(std::string(t.symbol))->second;
        });
      },
      [&] {
        hash = fresh_hash();
        return run(feed, [&](const Tick &t) -> Stats & {
          return hash.find(std::string(t.symbol))->second;
        });
      },
      [&] {
        index = fresh_index();
        return run(feed, [&](const Tick &t) -> Stats & { return index[t.id]; });
      },
  };
  const char *names[] = {"std::map<symbol>", "std::unordered_map<symbol>",
                         "std::vector[id]"};

  std::printf("%-28s %10s %12s %8s\n", "lookup", "ns/tick", "Mticks/s",
              "allocs");
  for (int w = 0; w < 3; ++w) {
    Result best{1e300, 0};
    for (int r = 0; r < reps; ++r) {
      Result res = ways[w]();
      if (res.sec < best.sec) best = res;
    }
    std::printf("%-28s %10.1f %12.1f %8ld\n", names[w], 1e9 * best.sec / nticks,
                nticks / best.sec / 1e6, best.allocs);
  }

  // The busiest tickers, from the vector[id] run (all three agree; checked).
  std::printf("\n%-6s %8s %9s %9s %9s %9s %9s %9s %7s %7s\n", "symbol",
              "trades", "last", "sma", "ewma", "vwap", "min", "max", "vol",
              "sigma");
  for (int i = 0; i < std::min(ntickers, 10); ++i) {
    Stats &s = index[i];
    std::printf("%-6s %8llu %9.2f %9.2f %9.2f %9.2f %9.2f %9.2f %6.1f%% "
                "%6.1f%%\n",
                tickers[i].symbol.c_str(), (unsigned long long)s.trades, s.last,
                s.sma(), s.ewma, s.vwap(), s.low(), s.high(), 100 * s.vol(),
                100 * tickers[i].sigma);
  }

  std::vector<History> hist = histories(tickers, feed);
  int bad = 0;
  for (int i = 0; i < ntickers; ++i) {
    const std::string &sym = tickers[i].symbol;
    Stats *all[] = {&tree.at(sym), &hash.at(sym), &index[i]};
    for (Stats *s : all) bad += !check(*s, hist[i], window);
  }
  std::printf("\ncheck: %s (%d tickers x 3 lookups against brute force)\n",
              bad ? "FAILED" : "ok", ntickers);
  return bad ? 1 : 0;
}
