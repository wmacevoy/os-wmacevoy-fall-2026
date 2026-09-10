//
// hello.c -- one worker thread says hello and hands back a value.
//
// The two threads print the same process id and different thread ids. That is
// the whole difference between a thread and a process, in two lines of output.
//

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

//
// Runs on the new thread. Its void * return value is the thread's exit
// status: pthread_join hands it to whoever is waiting.
//
static void *worker(void *arg)
{
  (void)arg;

  printf("hello from the worker: thread %#lx, process %d\n",
	 (unsigned long)(uintptr_t)pthread_self(), (int)getpid());

  // 42 has to travel as a void *, so it goes out through intptr_t -- the
  // integer type guaranteed to survive a round trip through a pointer.
  return (void *)(intptr_t)42;
}

int main(void)
{
  printf("main:                  thread %#lx, process %d\n",
	 (unsigned long)(uintptr_t)pthread_self(), (int)getpid());

  pthread_t worker_thread;
  int err = pthread_create(&worker_thread, NULL, worker, NULL);
  if (err != 0) {
    fprintf(stderr, "pthread_create: %s\n", strerror(err));
    return 1;
  }

  // Wait for the worker to finish, and collect what it returned.
  void *result;
  err = pthread_join(worker_thread, &result);
  if (err != 0) {
    fprintf(stderr, "pthread_join: %s\n", strerror(err));
    return 1;
  }

  printf("main:                  the worker returned %d\n",
	 (int)(intptr_t)result);

  return 0;
}
