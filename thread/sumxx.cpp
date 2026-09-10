//
// sumxx.cpp -- use threads to add up sum(f(k),k=0..n-1)
//

#include <thread>
#include <iostream>
#include <vector>
#include <memory>
#include <string>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <chrono>

// command line argument parsing
struct Args {
  int argc;
  const char **argv;
  
  int n;
  int threads;
  bool unroll;
  bool ok;

  void defaults() {
    n = 1'000'000'000;
    threads = std::thread::hardware_concurrency();
    unroll = false;
    ok = true;
  }

  bool sane() {
    if (!(n > 0 && n <= 1'000'000'000)) return false;
    if (!(threads > 0 && threads <= 1'000)) return false;
    return true;
  }
  
  Args(int _argc, const char **_argv) : argc(_argc), argv(_argv) {
    defaults();
    ok = parse() && sane();
  }

  bool parse() {
    if (argv[0] == NULL) return true;
    for (int argi=1; argv[argi] != NULL; ++argi) {
      std::string arg = argv[argi];
      if (arg == "--n" && argv[argi+1] != NULL) {
	n = atoi(argv[++argi]);
	continue;
      }
      if (arg == "--threads" && argv[argi+1] != NULL) {
	threads = atoi(argv[++argi]);
	continue;
      }
      if (arg == "--unroll") {
	unroll = true;
	continue;
      }
      if (arg == "--help") {
	usage(std::cout);
	continue;
      }
      return false;
    }
    return true;
  }
  
  void usage(std::ostream &err = std::cerr) {
    Args defaults(0,NULL);
    err << "usage: sumxx"
	<< " [--n # (default " << defaults.n << ")]"
	<< " [--threads # (default " << defaults.threads << ")]"
	<< " [--time]"
	<< " [--help]"
	<< std::endl;
  }
};

//
// One worker: sums f(k) over k in [a,b).
//
template <typename F>
struct Worker {
  const Args &args;
  const int a,b;
  F f;
  double ans;
  std::thread thread; // construct *last*  after parameters

  void run() {
    double s = 0;
    int k = a;

    if (args.unroll) {
      while (k + 4 < b) {
	double v[4] = {f(k+0),f(k+1),f(k+2),f(k+3)};
	s += v[0]+v[1]+v[2]+v[3];
	k += 4;
      }
    }
    while (k < b) {
      s += f(k);
      k += 1;
    }
    ans = s;
  }

  void join() {
    thread.join();
  }
  
  Worker(const Args &_args, int _a, int _b, F _f)
    : args(_args), a(_a), b(_b), f(_f), ans(0), thread(&Worker::run,this) {}
  
  ~Worker() {
    if (thread.joinable()) {
      thread.join();
    }
  }
};

//  sum(f(k),k=0..n-1) using # threads...
template <typename F>
double threaded_sum(const Args &args, F f) {
  // since vector can reallocate, make vector of pointers to workers instead...
  std::vector < std::unique_ptr<Worker<F> > > workers;
    
  // set up and start all workers...

  for (long t = 0; t<args.threads; ++t) {
    int a = args.n*t/args.threads;
    int b = args.n*(t+1)/args.threads;
    workers.push_back(std::make_unique<Worker<F> >(args,a,b,f));
  }
  for (auto &worker : workers) worker->join();
  double sum = 0;
  for (auto &worker : workers) sum += worker->ans;

  return sum;
}

int main(int argc, const char *argv[])
{
  Args args(argc,argv);

  if (!args.ok) {
    args.usage();
    return 1;
  }

  auto t0 = std::chrono::steady_clock::now();

  double total = threaded_sum(args, [](int k) { return sqrt(k); });

  auto t1 = std::chrono::steady_clock::now();
  double secs = std::chrono::duration<double>(t1-t0).count();

  std::cout
    << "sum(sqrt(k),k=0.." << (args.n-1) << ")="
    << std::setprecision(17) << total
    << " on " << args.threads << " threads"
    << " in " << std::setprecision(4) <<  secs/args.n*1e9 << " ns/element"
    << std::endl;
  
  return 0;
}
