//
// stats.cpp -- polymorphic statistics as flyweights over std::vector.
//
// A Series is one ticker's price history: a symbol and a std::vector<double>.
//
// A Stat knows *how* to compute one number from a history (the average of
// the last 5 prices, the lowest price, ...) but holds no history itself.
// That makes it a flyweight: one small, immutable object shared by every
// Series. Its intrinsic state (e.g. the window size) lives in the Stat; the
// extrinsic state (the prices) is handed in on each call.
//
// Stats are polymorphic: Stat is an abstract base with a virtual
// operator(), and each kind of statistic is a subclass.
//
// The Flyweights factory makes sure there is only ever one of each: asking
// for sma(5) twice returns the same object.
//
// usage: ./stats
//

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

// ---- the data -------------------------------------------------------------

struct Series {
  std::string symbol;
  std::vector<double> prices; // oldest first
};

// ---- the flyweights -------------------------------------------------------

class Stat {
public:
  virtual ~Stat() {}
  virtual std::string name() const = 0;
  // The statistic over prices; prices is never empty.
  virtual double operator()(const std::vector<double> &prices) const = 0;
};

// Most stats look only at the last `window` prices (or all, if fewer).
class WindowStat : public Stat {
protected:
  std::size_t window;
  explicit WindowStat(std::size_t w) : window(w) {}
  std::vector<double>::const_iterator first(const std::vector<double> &p) const {
    return p.end() - std::min(window, p.size());
  }
};

class Last : public Stat {
public:
  std::string name() const override { return "last"; }
  double operator()(const std::vector<double> &p) const override {
    return p.back();
  }
};

class Sma : public WindowStat {
public:
  explicit Sma(std::size_t w) : WindowStat(w) {}
  std::string name() const override { return "sma" + std::to_string(window); }
  double operator()(const std::vector<double> &p) const override {
    double sum = 0;
    for (auto i = first(p); i != p.end(); ++i) sum += *i;
    return sum / (p.end() - first(p));
  }
};

class Min : public WindowStat {
public:
  explicit Min(std::size_t w) : WindowStat(w) {}
  std::string name() const override { return "min" + std::to_string(window); }
  double operator()(const std::vector<double> &p) const override {
    return *std::min_element(first(p), p.end());
  }
};

class Max : public WindowStat {
public:
  explicit Max(std::size_t w) : WindowStat(w) {}
  std::string name() const override { return "max" + std::to_string(window); }
  double operator()(const std::vector<double> &p) const override {
    return *std::max_element(first(p), p.end());
  }
};

// Exponentially weighted moving average over the whole history.
class Ewma : public Stat {
  double alpha;

public:
  explicit Ewma(double a) : alpha(a) {}
  std::string name() const override {
    char buf[32];
    std::snprintf(buf, sizeof buf, "ewma%.2f", alpha);
    return buf;
  }
  double operator()(const std::vector<double> &p) const override {
    double e = p.front();
    for (double x : p) e += alpha * (x - e);
    return e;
  }
};

// Standard deviation of the percent change from one price to the next.
class Volatility : public WindowStat {
public:
  explicit Volatility(std::size_t w) : WindowStat(w) {}
  std::string name() const override { return "vol" + std::to_string(window); }
  double operator()(const std::vector<double> &p) const override {
    std::vector<double> changes;
    for (auto i = first(p); i != p.end(); ++i)
      if (i != p.begin()) changes.push_back(100 * (*i / *(i - 1) - 1));
    if (changes.size() < 2) return 0;
    double mean = 0, var = 0;
    for (double c : changes) mean += c / changes.size();
    for (double c : changes) var += (c - mean) * (c - mean);
    return std::sqrt(var / (changes.size() - 1));
  }
};

// Hands out one shared instance per distinct stat.
class Flyweights {
  std::map<std::string, std::unique_ptr<Stat>> pool;

  const Stat &intern(std::unique_ptr<Stat> made) {
    std::unique_ptr<Stat> &slot = pool[made->name()];
    if (!slot) slot = std::move(made);
    return *slot;
  }

public:
  const Stat &last() { return intern(std::make_unique<Last>()); }
  const Stat &sma(std::size_t w) { return intern(std::make_unique<Sma>(w)); }
  const Stat &min(std::size_t w) { return intern(std::make_unique<Min>(w)); }
  const Stat &max(std::size_t w) { return intern(std::make_unique<Max>(w)); }
  const Stat &ewma(double a) { return intern(std::make_unique<Ewma>(a)); }
  const Stat &vol(std::size_t w) {
    return intern(std::make_unique<Volatility>(w));
  }
  std::size_t size() const { return pool.size(); }
};

// ---- demo -----------------------------------------------------------------

int main() {
  std::vector<Series> market = {
      {"ACME", {10.00, 10.10, 10.05, 10.20, 10.40, 10.35, 10.50, 10.45}},
      {"BOLT", {52.00, 51.20, 50.80, 51.50, 49.90, 50.40}},
      {"CRUX", {3.10, 3.12, 3.11, 3.15, 3.09, 3.20, 3.18, 3.25, 3.30}},
  };

  Flyweights fly;
  std::vector<const Stat *> columns = {
      &fly.last(),   &fly.sma(3),  &fly.sma(5), &fly.ewma(0.5),
      &fly.min(5),   &fly.max(5),  &fly.vol(5),
  };
  // Asking again hands back the very same object.
  std::printf("sma(3) shared: %s\n", &fly.sma(3) == columns[1] ? "yes" : "no");

  std::printf("\n%-6s", "symbol");
  for (const Stat *s : columns) std::printf(" %9s", s->name().c_str());
  std::printf("\n");
  for (const Series &series : market) {
    std::printf("%-6s", series.symbol.c_str());
    for (const Stat *s : columns) std::printf(" %9.3f", (*s)(series.prices));
    std::printf("\n");
  }

  std::printf("\n%zu series x %zu stats = %zu values, from %zu shared Stat"
              " objects\n",
              market.size(), columns.size(), market.size() * columns.size(),
              fly.size());
  return 0;
}
