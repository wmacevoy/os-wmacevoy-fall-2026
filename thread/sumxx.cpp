//
// sumxx.cpp -- use threads to add up sum(f(k),k=0..n-1)
//

#include <thread>
#include <iostream>
#include <vector>
#include <memory>
#include <string>
#include <functional>
#include <cmath>
#include <cstdlib>
#include <iomanip>

struct Worker {
  const int a,b;
  std::function<double (int)> f;
  double ans;
  std::thread thread; // construct *last*  after parameters

  void run() {
    double s = 0;
    for (int k = a; k < b; ++k) s += f(k);
    ans = s;
  }

  void join() {
    thread.join();
  }
  
  Worker(int _a, int _b, std::function<double (int)> _f)
    : a(_a), b(_b), f(_f), ans(0), thread(&Worker::run,this) {}
  
  ~Worker() {
    if (thread.joinable()) {
      thread.join();
    }
  }
};

//  sum(f(k),k=0..n-1) using # threads...
double threaded_sum(std::function<double (int)> f,int n, int threads) {
  // since vector can reallocate, make vector of pointers to workers instead...
  std::vector < std::unique_ptr<Worker> > workers;
    
  // set up and start all workers...
  // t is long so n*t is 64-bit: n up to 1e9 times t up to 1000 overflows an
  // int, and the wrapped negative range makes sqrt() return nan.
  for (long t = 0; t<threads; ++t) {
    int a = n*t/threads;
    int b = n*(t+1)/threads;
    workers.push_back(std::make_unique<Worker>(a,b,f));
  }
  for (auto &worker : workers) worker->join();
  double sum = 0;
  for (auto &worker : workers) sum += worker->ans;

  return sum;
}


// command line argument parsing
struct Args {
  int argc;
  const char **argv;
  
  int n = 1000000;
  int threads = 8;
  
  Args(int _argc, const char **_argv) : argc(_argc), argv(_argv) {}

  bool parse() {
    for (int argi=1; argv[argi] != NULL; ++argi) {
      std::string arg = argv[argi];
      if (arg == "--n" && argv[argi+1] != NULL) {
	n = atoi(argv[++argi]);
	if (n <= 0 || n > 1'000'000'000) {
	  return false;
	}
	continue;
      }
      if (arg == "--threads" && argv[argi+1] != NULL) {
	threads = atoi(argv[++argi]);
	if (threads <= 0 || threads > 1000) {
	  return false;
	}
	continue;
      }
      return false;
    }
    return true;
  }
  
  void usage(std::ostream &err = std::cerr) {
    err << "usage: " << argv[0] << " [--n <#>] [--threads <#>]" << std::endl;
  }
};
  

int main(int argc, const char *argv[])
{
  Args args(argc,argv);

  if (!args.parse()) {
    args.usage();
    return 1;
  }

  // sum sqrt(k), k = 0,..,args.n-1 using args.threads threads
  double total = threaded_sum(
			      [](int k) { return sqrt(k); },
			      args.n,
			      args.threads);

  // setprecision(17), not the default 6: a double holds about 17 significant
  // digits, and the interesting part of this answer is in the last few of them
  // -- run it at different --threads and watch the tail digits move.
  std::cout
    << "sum(sqrt(k),k=0.." << (args.n-1) << ")="
    << std::setprecision(17) << total
    << " on " << args.threads << " threads" << std::endl;

  return 0;
}
  


