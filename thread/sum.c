//
// sum.c -- use threads to add up sum(sqrt(k),k=0..n-1)
//

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

// [a,b) is the sum range for this worker's job
// ans = sum(sqrt(k),k=a..b-1)
typedef struct {
  int a,b;
  double ans;
} sum_t;

//
// worker's problem is a sub-sum sqrt(k) for k in [a,b)
// arg is actually a pointer to a sum_t, and worker
// returns the same pointer, now with ans filled in.
//
static void *worker(void *arg)
{
  sum_t *sum = (sum_t*)arg;

  // get parameters from job
  int a = sum->a, b = sum->b;
  // compute the sum using local variables
  double s = 0;
  for (int k=a; k<b; ++k) {
    s += sqrt(k);
  }
  // record the sum as a result
  sum->ans=s;

  // return pointer to result
  return (void *) sum;
}

int main(int argc, const char *argv[])
{
  int n = 1000000;
  int threads = 8;

  // process --n <#>, --threads <#>in cli
  for (int argi=1; argi<argc; ++argi) {
    if (strcmp(argv[argi],"--n")==0 && argv[argi+1] != NULL) {
      n = atoi(argv[++argi]);
      if (n > 0 && n <= 1000*1000*1000) {
	continue;
      }
    }
    else if (strcmp(argv[argi],"--threads")==0 && argv[argi+1] != NULL) {
      threads = atoi(argv[++argi]);
      if (threads > 0 && threads <= 1000) {
	continue;
      }
    }
    fprintf(stderr,
	    "usage: %s [--n <#>] [--threads <#>]\n",
	    argv[0]);

    return 1;
  }

  pthread_t *worker_threads = (pthread_t *) malloc(sizeof(pthread_t)*threads);
  if (worker_threads == NULL) {
    fprintf(stderr,"worker thread array allocation failed\n");
    return 1;
  }
  sum_t *sums = (sum_t*) malloc(sizeof(sum_t)*threads);
  if (sums == NULL) {
    fprintf(stderr,"sum result array allocation failed\n");
    return 1;
  }

  // initialize work (sum) and workers...
  //
  // t is long so that n*t is done in 64 bits.  --n allows 1e9 and --threads
  // allows 1000, and that product overflows an int by a factor of 500: the
  // wrapped value comes out negative, a<b picks up a negative range, and the
  // answer is a quiet nan.  Widening the counter fixes it with no cast.
  for (long t = 0; t<threads; ++t) {
    sums[t].a = n*t/threads;
    sums[t].b = n*(t+1)/threads;
    sums[t].ans = 0;

    int err = pthread_create(&worker_threads[t], NULL, worker, &sums[t]);
    if (err != 0) {
      fprintf(stderr, "pthread_create: %s\n", strerror(err));
      return 1;
    }
  }

  // join workers and accumulate result
  double total = 0;
  for (int t = 0; t<threads; ++t) {
    // Wait for the worker to finish, and collect what it returned.
    sum_t *result = NULL;
    int err = pthread_join(worker_threads[t], (void**)&result);
    if (err != 0) {
      fprintf(stderr, "pthread_join: %s\n", strerror(err));
      return 1;
    }
    total = total + result->ans;
  }

  // %.17g, not %lg: a double holds about 17 significant digits, and the
  // interesting part of this answer is in the last few of them -- run it at
  // different --threads and watch the tail digits move.
  printf("sum(sqrt(k),k=0..%d)=%.17g on %d threads\n", n-1, total, threads);

  free(worker_threads);
  free(sums);

  return 0;
}
