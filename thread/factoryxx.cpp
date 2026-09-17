//
// factoryxx.cpp -- factory.py in C++17: one std::thread per worker.
//
// A machine builds boxes, forklifts carry them through the dock door, and
// delivery trucks wait in a loading bay until they are full, then drive off.
// Each worker is a plain loop that reads like its job description -- see
// forklift() and truck() -- and the OS scheduler does the interleaving.
//
// Anything touched by more than one thread is guarded by a std::mutex:
//
//   Floor::lock   the pile of finished boxes on the factory floor
//   door          the dock door: one forklift fits through at a time
//   Dock::lock    the loading bays: which truck is parked where, how much room
//   print_lock    stdout, so log lines never mix
//
// A thread never holds two of the first three at once, and nothing is locked
// while print_lock is held, so there is no deadlock (compare philosophers.c).
//
// Floor and Dock are monitors, the usual C++ shape for shared state: a class
// keeps its data, its mutex and its condition variables private, and each
// public member function holds the mutex for its whole body. Code outside the
// class cannot reach the data without the lock.
//
// Nothing calls lock() or unlock() directly. A lock object locks in its
// constructor and unlocks in its destructor, even on an early return or an
// exception: std::scoped_lock for a plain critical section, std::unique_lock
// where we wait, because wait() has to unlock and relock it.
//
// A thread that has to wait -- for a box, an empty bay, a full truck -- sleeps
// on a std::condition_variable. cv.wait(lock, ready) is the standard wait
// loop, written out for you:
//
//   while (!ready()) {
//     cv.wait(lock);  // unlock, sleep until notified, lock again
//   }
//
// It is a loop because another thread may get there first, and wait() may
// even return when nobody notified.
//
// Whoever changes the data calls notify_all() on the condition variable that
// change might satisfy:
//
//   Floor::changed        a box was built, or the machine finished
//   Dock::has_room        a truck parked with room to fill
//   Dock::has_empty_bay   a truck pulled out
//   Bay::full             the truck in this bay just got its last box
//
// notify_all() wakes every waiter. Each one rechecks its wait loop, and any
// that still has nothing to do goes back to sleep. That keeps correctness easy
// to see: if every loop tests the right thing and every change notifies, no
// thread sleeps through what it was waiting for. notify_one() wakes just one
// waiter, which saves wakeups but is only correct if the one it wakes is sure
// to use the change -- a subtle argument, and easy to get wrong.
//
// A condition variable waits with one mutex. A forklift waits for room on
// *any* truck, so one mutex guards all of the bays, and the dock's and the
// bays' condition variables all wait with it.
//

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

const auto start_time = std::chrono::steady_clock::now();
std::mutex print_lock;

// Each thread has its own copy of a thread_local variable, so say() and
// busy() can tell who is calling without being told.
thread_local std::string thread_name = "main";
thread_local std::mt19937 rng{std::random_device{}()};

// Print one line of the story. (Not log(): <cmath> has that name.)
template <typename... Parts>
void say(const Parts &...parts)
{
  std::scoped_lock hold(print_lock);
  std::chrono::duration<double> stamp =
    std::chrono::steady_clock::now() - start_time;
  std::ostringstream line;
  line << std::fixed << std::setprecision(2) << std::setw(5) << stamp.count()
       << "s  " << std::left << std::setw(10) << thread_name << ' ';
  (line << ... << parts);
  std::cout << line.str() << std::endl;
}

// Spend a random number of seconds, between lo and hi, doing the job.
void busy(double lo, double hi)
{
  std::uniform_real_distribution<double> seconds(lo, hi);
  std::this_thread::sleep_for(std::chrono::duration<double>(seconds(rng)));
}

// Finished boxes waiting to be picked up.
class Floor {
public:
  // The machine sets down a box it just built.
  void add_box()
  {
    std::scoped_lock hold(lock);
    ++boxes;
    ++made;
    say("built box #", made, " (", boxes, " on the floor)");
    changed.notify_all();
  }

  // The machine has built its last box.
  void finish()
  {
    std::scoped_lock hold(lock);
    done = true;
    changed.notify_all();  // every waiting forklift can go park
  }

  // Pick up a box. Returns false once there will never be another.
  bool take_box()
  {
    std::unique_lock hold(lock);
    changed.wait(hold, [this] { return boxes > 0 || done; });
    if (boxes == 0) {
      return false;
    }
    --boxes;
    say("picks up a box (", boxes, " left on the floor)");
    return true;
  }

  // For the final report, once every worker has been joined and nobody else
  // can touch the floor. That is why they don't lock.
  int built() const { return made; }
  int left() const { return boxes; }

private:
  std::mutex lock;
  std::condition_variable changed;
  int boxes = 0;      // on the floor right now
  int made = 0;       // built so far
  bool done = false;  // the machine has built its last box
};

// A loading bay on the dock. Holds at most one truck. The dock's lock
// guards it.
struct Bay {
  int number = 0;
  std::condition_variable full;
  std::optional<std::string> truck;  // name of the parked truck, if any
  int room = 0;                      // boxes the parked truck can still take
  int shipped = 0;                   // boxes that have left from this bay
};

// The loading bays. One lock guards all of them.
class Dock {
public:
  // A condition_variable can be neither copied nor moved, so neither can a
  // Bay: the vector is built at full size, once, and never grows.
  explicit Dock(int count) : bays(count)
  {
    for (int i = 0; i < count; ++i) {
      bays[i].number = i + 1;
    }
  }

  // A truck backs into any empty bay. Returns the bay's number.
  int park(int capacity)
  {
    std::unique_lock hold(lock);
    has_empty_bay.wait(hold, [this] { return any(is_empty); });
    Bay &bay = first(is_empty);
    bay.truck = thread_name;
    bay.room = capacity;
    say("parks in bay ", bay.number);
    has_room.notify_all();
    return bay.number;
  }

  // A forklift puts the box it is carrying on any parked truck with room.
  void load_truck()
  {
    std::unique_lock hold(lock);
    has_room.wait(hold, [this] { return any(has_space); });
    Bay &bay = first(has_space);
    --bay.room;
    say("loads ", *bay.truck, " in bay ", bay.number,
        " (room for ", bay.room, " more)");
    if (bay.room == 0) {
      bay.full.notify_all();  // wake the truck parked here
    }
  }

  // A truck waits in its bay until it is full, then pulls out.
  void leave_when_full(int number, int capacity)
  {
    std::unique_lock hold(lock);
    Bay &bay = bays[number - 1];
    bay.full.wait(hold, [&bay] { return bay.room == 0; });
    bay.truck.reset();
    bay.shipped += capacity;
    say("is full, pulls out of bay ", bay.number);
    has_empty_bay.notify_all();
  }

  // For the final report, once every worker has been joined.
  int shipped() const
  {
    int total = 0;
    for (const Bay &bay : bays) {
      total += bay.shipped;
    }
    return total;
  }

private:
  static bool is_empty(const Bay &bay) { return !bay.truck; }
  static bool has_space(const Bay &bay) { return bay.room > 0; }

  // Call these with the lock held.
  bool any(bool (*test)(const Bay &)) const
  {
    return std::any_of(bays.begin(), bays.end(), test);
  }
  Bay &first(bool (*test)(const Bay &))
  {
    return *std::find_if(bays.begin(), bays.end(), test);
  }

  std::mutex lock;
  std::condition_variable has_room;
  std::condition_variable has_empty_bay;
  std::vector<Bay> bays;
};

void drive_through(std::mutex &door, std::string_view where)
{
  std::scoped_lock hold(door);
  say("drives through the door ", where);
  busy(0.05, 0.10);
}

//
// The workers. Each function is the whole life of one thread.
//

void machine(Floor &floor, int total)
{
  for (int i = 0; i < total; ++i) {
    busy(0.05, 0.15);
    floor.add_box();
  }
  floor.finish();
  say("finished the order");
}

void forklift(Floor &floor, std::mutex &door, Dock &dock)
{
  while (floor.take_box()) {
    drive_through(door, "out to the dock");
    dock.load_truck();
    drive_through(door, "back to the floor");
  }
  say("floor is clear, parking");
}

void truck(Dock &dock, int trips, int capacity)
{
  for (int trip = 1; trip <= trips; ++trip) {
    int bay = dock.park(capacity);
    dock.leave_when_full(bay, capacity);
    busy(0.4, 0.8);  // out on delivery
    say("delivered load ", trip, " of ", trips);
  }
  say("done for the day");
}

// Start a thread that knows its own name.
template <typename Job>
std::thread hire(const std::string &name, Job job)
{
  std::thread worker([name, job] {
    thread_name = name;
    job();
  });
  say("started ", name, " as thread ", worker.get_id());
  return worker;
}

struct Args {
  int forklifts = 3;
  int trucks = 3;
  int bays = 2;
  int trips = 2;     // deliveries each truck makes
  int capacity = 3;  // boxes in a full truck
};

// Reads --<option> <#> pairs into args. Returns false if anything is off.
bool parse(int argc, const char *argv[], Args &args)
{
  const std::map<std::string_view, int *> options = {
    {"--forklifts", &args.forklifts},
    {"--trucks", &args.trucks},
    {"--bays", &args.bays},
    {"--trips", &args.trips},
    {"--capacity", &args.capacity},
  };
  for (int argi = 1; argi < argc; argi += 2) {
    auto option = options.find(argv[argi]);
    if (option == options.end() || argi + 1 >= argc) {
      return false;
    }
    int &count = *option->second;
    count = std::atoi(argv[argi + 1]);
    if (count < 1 || count > 1000) {
      return false;
    }
  }
  return true;
}

}  // namespace

int main(int argc, const char *argv[])
{
  Args args;
  if (!parse(argc, argv, args)) {
    std::cerr << "usage: " << argv[0]
              << " [--forklifts <#>] [--trucks <#>] [--bays <#>]"
              << " [--trips <#>] [--capacity <#>]\n"
              << "  every count is from 1 to 1000" << std::endl;
    return 1;
  }

  // Build exactly as many boxes as the trucks will carry away, so every
  // worker finishes and the program ends on its own.
  const int total = args.trucks * args.trips * args.capacity;

  Floor floor;
  std::mutex door;
  Dock dock(args.bays);

  std::vector<std::thread> workers;
  workers.push_back(hire("machine", [&] { machine(floor, total); }));
  for (int n = 1; n <= args.forklifts; ++n) {
    workers.push_back(hire("forklift" + std::to_string(n),
                           [&] { forklift(floor, door, dock); }));
  }
  for (int n = 1; n <= args.trucks; ++n) {
    workers.push_back(hire("truck" + std::to_string(n),
                           [&] { truck(dock, args.trips, args.capacity); }));
  }

  for (std::thread &worker : workers) {
    worker.join();
  }

  // Every worker has exited, so nobody else can touch the shared state now:
  // reading it without the locks is safe.
  const int shipped = dock.shipped();
  std::cout << "\nordered " << total << ", built " << floor.built()
            << ", shipped " << shipped << ", " << floor.left()
            << " left on the floor" << std::endl;
  bool ok = floor.built() == total && shipped == total && floor.left() == 0;
  return ok ? 0 : 1;
}
