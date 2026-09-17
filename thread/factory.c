//
// factory.c -- factory.py in C: one pthread per worker.
//
// A machine builds boxes, forklifts carry them through the dock door, and
// delivery trucks wait in a loading bay until they are full, then drive off.
// Each worker is a plain loop that reads like its job description -- see
// forklift() and truck() -- and the OS scheduler does the interleaving.
//
// Anything touched by more than one thread is guarded by a mutex:
//
//   floor.lock   the pile of finished boxes on the factory floor
//   door         the dock door: one forklift fits through at a time
//   dock.lock    the loading bays: which truck is parked where, how much room
//   print_lock   stdout, so log lines never mix
//
// A thread never holds two of the first three at once, and nothing is locked
// while print_lock is held, so there is no deadlock (compare philosophers.c).
//
// A thread that has to wait -- for a box, an empty bay, a full truck -- sleeps
// on a condition variable. pthread_cond_wait() unlocks the mutex, sleeps until
// another thread signals, and locks the mutex again before it returns. Every
// wait has the same shape:
//
//   pthread_mutex_lock(&lock);
//   while (!<what we need>) {
//     pthread_cond_wait(&cond, &lock);
//   }
//   <use it>
//   pthread_mutex_unlock(&lock);
//
// Always a while, never an if: another thread may get there first, and
// pthread_cond_wait() may even return when nobody signaled.
//
// Whoever changes the data calls pthread_cond_broadcast() on the condition
// variable that change might satisfy:
//
//   floor.changed        a box was built, or the machine finished
//   dock.has_room        a truck parked with room to fill
//   dock.has_empty_bay   a truck pulled out
//   bay.full             the truck in this bay just got its last box
//
// A broadcast wakes every waiter. Each one rechecks its while loop, and any
// that still has nothing to do goes back to sleep. That keeps correctness easy
// to see: if every loop tests the right thing and every change broadcasts, no
// thread sleeps through what it was waiting for. pthread_cond_signal() wakes
// just one waiter, which saves wakeups but is only correct if the one it wakes
// is sure to use the change -- a subtle argument, and easy to get wrong.
//
// A pthread condition variable is not tied to a mutex when it is created.
// You pass the mutex to every wait, and it must be the same mutex every time.
// A forklift waits for room on *any* truck, so one mutex, dock.lock, guards
// all of the bays, and the dock's and the bays' condition variables all wait
// with it.
//

#define _XOPEN_SOURCE 700  // for erand48() and nanosleep()

#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Finished boxes waiting to be picked up.
typedef struct {
  pthread_mutex_t lock;
  pthread_cond_t changed;
  int boxes;  // on the floor right now
  int made;   // built so far
  bool done;  // the machine has built its last box
} floor_t;

// A loading bay on the dock. Holds at most one truck.
typedef struct {
  int number;
  pthread_cond_t full;  // waits with the dock's lock
  const char *truck;    // name of the parked truck, or NULL
  int room;             // boxes the parked truck can still take
  int shipped;          // boxes that have left from this bay
} bay_t;

// The loading bays. One lock guards all of them.
typedef struct {
  pthread_mutex_t lock;
  pthread_cond_t has_room;
  pthread_cond_t has_empty_bay;
  int count;
  bay_t *bays;
} dock_t;

// Everything the workers share, and the order they are filling.
typedef struct {
  floor_t floor;
  pthread_mutex_t door;
  dock_t dock;
  int total;     // boxes to build
  int trips;     // deliveries each truck makes
  int capacity;  // boxes in a full truck
} factory_t;

// One thread: who it is, what it does, and where.
typedef struct {
  char name[32];
  void (*job)(factory_t *factory);
  factory_t *factory;
  unsigned short rng[3];  // this worker's own erand48() state
  pthread_t thread;
} worker_t;

static struct timespec start_time;
static pthread_mutex_t print_lock = PTHREAD_MUTEX_INITIALIZER;

// The worker running on this thread, or NULL on the main thread. Each thread
// has its own copy of a _Thread_local variable, so say() and busy() can tell
// who is calling without being told.
static _Thread_local worker_t *self = NULL;

// Print one line of the story. (Not log(): the math library has that name.)
static void say(const char *format, ...)
{
  pthread_mutex_lock(&print_lock);

  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  double stamp = (now.tv_sec - start_time.tv_sec)
    + 1e-9 * (now.tv_nsec - start_time.tv_nsec);
  printf("%5.2fs  %-10s ", stamp, self != NULL ? self->name : "main");

  va_list args;
  va_start(args, format);
  vprintf(format, args);
  va_end(args);

  printf("\n");
  fflush(stdout);
  pthread_mutex_unlock(&print_lock);
}

// Spend a random number of seconds, between lo and hi, doing the job.
// erand48() keeps its state in the worker, so threads never share it.
static void busy(double lo, double hi)
{
  double secs = lo + (hi - lo) * erand48(self->rng);
  struct timespec nap = {
    .tv_sec = (time_t)secs,
    .tv_nsec = (long)((secs - (time_t)secs) * 1e9),
  };
  nanosleep(&nap, NULL);
}

//
// The steps, defined below. Each one waits as long as it has to, then does
// its job.
//
static bool take_box(floor_t *floor);
static void drive_through(pthread_mutex_t *door, const char *where);
static void load_truck(dock_t *dock);
static bay_t *park(dock_t *dock, int capacity);
static void leave_when_full(dock_t *dock, bay_t *bay, int capacity);

//
// The workers. Each function is the whole life of one thread.
//

static void machine(factory_t *factory)
{
  floor_t *floor = &factory->floor;
  for (int i = 0; i < factory->total; ++i) {
    busy(0.05, 0.15);
    pthread_mutex_lock(&floor->lock);
    floor->boxes += 1;
    floor->made += 1;
    say("built box #%d (%d on the floor)", floor->made, floor->boxes);
    pthread_cond_broadcast(&floor->changed);
    pthread_mutex_unlock(&floor->lock);
  }
  pthread_mutex_lock(&floor->lock);
  floor->done = true;
  pthread_cond_broadcast(&floor->changed);  // every waiting forklift can go park
  pthread_mutex_unlock(&floor->lock);
  say("finished the order");
}

static void forklift(factory_t *factory)
{
  while (take_box(&factory->floor)) {
    drive_through(&factory->door, "out to the dock");
    load_truck(&factory->dock);
    drive_through(&factory->door, "back to the floor");
  }
  say("floor is clear, parking");
}

static void truck(factory_t *factory)
{
  for (int trip = 1; trip <= factory->trips; ++trip) {
    bay_t *bay = park(&factory->dock, factory->capacity);
    leave_when_full(&factory->dock, bay, factory->capacity);
    busy(0.4, 0.8);  // out on delivery
    say("delivered load %d of %d", trip, factory->trips);
  }
  say("done for the day");
}

//
// The steps.
//

// Pick up a box. Returns false once there will never be another.
static bool take_box(floor_t *floor)
{
  pthread_mutex_lock(&floor->lock);
  while (floor->boxes == 0 && !floor->done) {
    pthread_cond_wait(&floor->changed, &floor->lock);
  }
  bool got_one = floor->boxes > 0;
  if (got_one) {
    floor->boxes -= 1;
    say("picks up a box (%d left on the floor)", floor->boxes);
  }
  pthread_mutex_unlock(&floor->lock);
  return got_one;
}

static void drive_through(pthread_mutex_t *door, const char *where)
{
  pthread_mutex_lock(door);
  say("drives through the door %s", where);
  busy(0.05, 0.10);
  pthread_mutex_unlock(door);
}

// The first bay whose truck has room, or NULL. Call with dock->lock held.
static bay_t *bay_with_room(dock_t *dock)
{
  for (int i = 0; i < dock->count; ++i) {
    if (dock->bays[i].room > 0) {
      return &dock->bays[i];
    }
  }
  return NULL;
}

// The first bay with no truck in it, or NULL. Call with dock->lock held.
static bay_t *empty_bay(dock_t *dock)
{
  for (int i = 0; i < dock->count; ++i) {
    if (dock->bays[i].truck == NULL) {
      return &dock->bays[i];
    }
  }
  return NULL;
}

// Put the box we are carrying on any parked truck with room.
static void load_truck(dock_t *dock)
{
  pthread_mutex_lock(&dock->lock);
  bay_t *bay;
  while ((bay = bay_with_room(dock)) == NULL) {
    pthread_cond_wait(&dock->has_room, &dock->lock);
  }
  bay->room -= 1;
  say("loads %s in bay %d (room for %d more)",
      bay->truck, bay->number, bay->room);
  if (bay->room == 0) {
    pthread_cond_broadcast(&bay->full);  // wake the truck parked here
  }
  pthread_mutex_unlock(&dock->lock);
}

// Back into any empty bay.
static bay_t *park(dock_t *dock, int capacity)
{
  pthread_mutex_lock(&dock->lock);
  bay_t *bay;
  while ((bay = empty_bay(dock)) == NULL) {
    pthread_cond_wait(&dock->has_empty_bay, &dock->lock);
  }
  bay->truck = self->name;
  bay->room = capacity;
  say("parks in bay %d", bay->number);
  pthread_cond_broadcast(&dock->has_room);
  pthread_mutex_unlock(&dock->lock);
  return bay;
}

static void leave_when_full(dock_t *dock, bay_t *bay, int capacity)
{
  pthread_mutex_lock(&dock->lock);
  while (bay->room > 0) {
    pthread_cond_wait(&bay->full, &dock->lock);
  }
  bay->truck = NULL;
  bay->shipped += capacity;
  say("is full, pulls out of bay %d", bay->number);
  pthread_cond_broadcast(&dock->has_empty_bay);
  pthread_mutex_unlock(&dock->lock);
}

//
// Setting up, and tearing down.
//

// Every thread starts here, then spends its whole life in its job.
static void *start_worker(void *arg)
{
  self = arg;
  self->job(self->factory);
  return NULL;
}

// Returns false if there is no memory for the bays.
static bool factory_init(factory_t *factory, int bays)
{
  factory->dock.bays = malloc(bays * sizeof(bay_t));
  if (factory->dock.bays == NULL) {
    return false;
  }
  factory->dock.count = bays;
  for (int i = 0; i < bays; ++i) {
    bay_t *bay = &factory->dock.bays[i];
    bay->number = i + 1;
    pthread_cond_init(&bay->full, NULL);
    bay->truck = NULL;
    bay->room = 0;
    bay->shipped = 0;
  }
  pthread_mutex_init(&factory->dock.lock, NULL);
  pthread_cond_init(&factory->dock.has_room, NULL);
  pthread_cond_init(&factory->dock.has_empty_bay, NULL);

  pthread_mutex_init(&factory->floor.lock, NULL);
  pthread_cond_init(&factory->floor.changed, NULL);
  factory->floor.boxes = 0;
  factory->floor.made = 0;
  factory->floor.done = false;

  pthread_mutex_init(&factory->door, NULL);
  return true;
}

static void factory_destroy(factory_t *factory)
{
  for (int i = 0; i < factory->dock.count; ++i) {
    pthread_cond_destroy(&factory->dock.bays[i].full);
  }
  free(factory->dock.bays);
  pthread_cond_destroy(&factory->dock.has_empty_bay);
  pthread_cond_destroy(&factory->dock.has_room);
  pthread_mutex_destroy(&factory->dock.lock);

  pthread_cond_destroy(&factory->floor.changed);
  pthread_mutex_destroy(&factory->floor.lock);

  pthread_mutex_destroy(&factory->door);
}

static int usage(const char *program)
{
  fprintf(stderr,
	  "usage: %s [--forklifts <#>] [--trucks <#>] [--bays <#>]"
	  " [--trips <#>] [--capacity <#>]\n"
	  "  every count is from 1 to 1000\n",
	  program);
  return 1;
}

int main(int argc, const char *argv[])
{
  clock_gettime(CLOCK_MONOTONIC, &start_time);

  int forklifts = 3;
  int trucks = 3;
  int bays = 2;
  int trips = 2;     // deliveries each truck makes
  int capacity = 3;  // boxes in a full truck

  struct {
    const char *flag;
    int *count;
  } options[] = {
    {"--forklifts", &forklifts},
    {"--trucks", &trucks},
    {"--bays", &bays},
    {"--trips", &trips},
    {"--capacity", &capacity},
  };
  const int noptions = sizeof(options) / sizeof(options[0]);

  // process --<option> <#> pairs
  for (int argi = 1; argi < argc; argi += 2) {
    int *count = NULL;
    for (int k = 0; k < noptions; ++k) {
      if (strcmp(argv[argi], options[k].flag) == 0) {
	count = options[k].count;
      }
    }
    if (count == NULL || argi + 1 >= argc) {
      return usage(argv[0]);
    }
    *count = atoi(argv[argi + 1]);
    if (*count < 1 || *count > 1000) {
      return usage(argv[0]);
    }
  }

  // Build exactly as many boxes as the trucks will carry away, so every
  // worker finishes and the program ends on its own.
  factory_t factory = {
    .total = trucks * trips * capacity,
    .trips = trips,
    .capacity = capacity,
  };
  if (!factory_init(&factory, bays)) {
    fprintf(stderr, "bay array allocation failed\n");
    return 1;
  }

  int nworkers = 1 + forklifts + trucks;
  worker_t *workers = malloc(nworkers * sizeof(worker_t));
  if (workers == NULL) {
    fprintf(stderr, "worker array allocation failed\n");
    return 1;
  }

  for (int i = 0; i < nworkers; ++i) {
    worker_t *worker = &workers[i];
    if (i == 0) {
      snprintf(worker->name, sizeof(worker->name), "machine");
      worker->job = machine;
    } else if (i <= forklifts) {
      snprintf(worker->name, sizeof(worker->name), "forklift%d", i);
      worker->job = forklift;
    } else {
      snprintf(worker->name, sizeof(worker->name), "truck%d", i - forklifts);
      worker->job = truck;
    }
    worker->factory = &factory;

    // A different starting point for each worker's random numbers.
    worker->rng[0] = (unsigned short)i;
    worker->rng[1] = (unsigned short)time(NULL);
    worker->rng[2] = 0x330E;

    int err = pthread_create(&worker->thread, NULL, start_worker, worker);
    if (err != 0) {
      fprintf(stderr, "pthread_create: %s\n", strerror(err));
      return 1;
    }
    say("started %s as thread %#lx",
	worker->name, (unsigned long)(uintptr_t)worker->thread);
  }

  for (int i = 0; i < nworkers; ++i) {
    int err = pthread_join(workers[i].thread, NULL);
    if (err != 0) {
      fprintf(stderr, "pthread_join: %s\n", strerror(err));
      return 1;
    }
  }

  // Every worker has exited, so nobody else can touch the shared state now:
  // reading it without the locks is safe.
  int shipped = 0;
  for (int i = 0; i < factory.dock.count; ++i) {
    shipped += factory.dock.bays[i].shipped;
  }
  printf("\nordered %d, built %d, shipped %d, %d left on the floor\n",
	 factory.total, factory.floor.made, shipped, factory.floor.boxes);
  bool ok = factory.floor.made == factory.total
    && shipped == factory.total
    && factory.floor.boxes == 0;

  free(workers);
  factory_destroy(&factory);

  return ok ? 0 : 1;
}
