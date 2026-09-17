//
// factoryxx_guarded.cpp -- factoryxx.cpp rebuilt on guarded.hpp.
//
// The same factory, with the same options and the same story. What changes
// is how a worker waits. factoryxx.cpp gives each monitor a mutex and
// condition variables, loops on wait(), and has every change notify the
// right condition variable. Here there are no condition variables and no
// notify calls. A step names the resources it needs and the condition it
// waits for:
//
//   auto hold = when([&] { return floor.boxes() > 0 || floor.done(); }, floor);
//
// when() holds the floor for the rest of the scope, once the condition is
// true. Until then it holds nothing and sleeps, and any worker that changes
// the floor wakes it to check again (see guarded.hpp).
//
// Floor and Dock are plain data with accessors. A const accessor starts with
// const_guard() and any other with guard(). Both throw if the calling thread
// does not hold the resource, so a forgotten lock is an error rather than a
// race.
//
//   floor        the pile of finished boxes on the factory floor
//   door         the dock door: one forklift fits through at a time
//   dock         the loading bays: which truck is parked where, how much room
//   print_lock   stdout, so log lines never mix
//
// Each step holds one resource at a time, so there is no deadlock. (A step
// that needed two would name both in one when(), which takes them in a
// fixed order.)
//
// stdout stays a plain std::mutex. say() is called while holding other
// resources, and a guard may not take a new resource while it holds others.
// That is safe here because print_lock is a leaf: nothing else is ever
// locked while it is held.
//

#include "guarded.hpp"

#include <chrono>
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

using guarded::Resource;
using guarded::reserve;
using guarded::when;

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
class Floor : public Resource {
public:
  int boxes() const { const_guard(); return on_floor; }
  int made() const { const_guard(); return built; }
  bool done() const { const_guard(); return finished; }

  void add_box() { guard(); ++on_floor; ++built; }
  void remove_box() { guard(); --on_floor; }
  void finish() { guard(); finished = true; }

private:
  int on_floor = 0;       // boxes on the floor right now
  int built = 0;          // built so far
  bool finished = false;  // the machine has built its last box
};

// The dock door has no data. Holding it is the whole point.
class Door : public Resource {};

// The loading bays, numbered from 1. Each holds at most one truck.
class Dock : public Resource {
public:
  explicit Dock(int count) : bays(count) {}

  // The first bay with no truck, or 0 if every bay is taken.
  int empty_bay() const
  {
    const_guard();
    return find([](const Bay &bay) { return !bay.truck; });
  }

  // The first bay whose truck has room, or 0 if there is none.
  int bay_with_room() const
  {
    const_guard();
    return find([](const Bay &bay) { return bay.room > 0; });
  }

  std::string truck(int number) const
  {
    const_guard();
    return *at(number).truck;
  }

  int room(int number) const { const_guard(); return at(number).room; }

  int shipped() const
  {
    const_guard();
    int total = 0;
    for (const Bay &bay : bays) {
      total += bay.shipped;
    }
    return total;
  }

  void park(int number, const std::string &truck, int capacity)
  {
    guard();
    at(number).truck = truck;
    at(number).room = capacity;
  }

  void load(int number)
  {
    guard();
    --at(number).room;
  }

  void pull_out(int number, int capacity)
  {
    guard();
    at(number).truck.reset();
    at(number).shipped += capacity;
  }

private:
  // Plain data: no condition variable, so a Bay copies like any struct.
  struct Bay {
    std::optional<std::string> truck;  // name of the parked truck, if any
    int room = 0;                      // boxes the parked truck can still take
    int shipped = 0;                   // boxes that have left from this bay
  };
  std::vector<Bay> bays;

  const Bay &at(int number) const { return bays[number - 1]; }
  Bay &at(int number) { return bays[number - 1]; }

  template <typename Test>
  int find(Test test) const
  {
    for (size_t i = 0; i < bays.size(); ++i) {
      if (test(bays[i])) {
        return static_cast<int>(i) + 1;
      }
    }
    return 0;
  }
};

//
// The steps. Each one says what it waits for, then does its job.
//

// Pick up a box. Returns false once there will never be another.
bool take_box(Floor &floor)
{
  auto hold = when([&] { return floor.boxes() > 0 || floor.done(); }, floor);
  if (floor.boxes() == 0) {
    return false;
  }
  floor.remove_box();
  say("picks up a box (", floor.boxes(), " left on the floor)");
  return true;
}

void drive_through(Door &door, std::string_view where)
{
  auto hold = reserve(door);
  say("drives through the door ", where);
  busy(0.05, 0.10);
}

// Put the box we are carrying on any parked truck with room.
void load_truck(Dock &dock)
{
  auto hold = when([&] { return dock.bay_with_room() != 0; }, dock);
  int bay = dock.bay_with_room();
  dock.load(bay);
  say("loads ", dock.truck(bay), " in bay ", bay,
      " (room for ", dock.room(bay), " more)");
}

// Back into any empty bay. Returns the bay's number.
int park(Dock &dock, int capacity)
{
  auto hold = when([&] { return dock.empty_bay() != 0; }, dock);
  int bay = dock.empty_bay();
  dock.park(bay, thread_name, capacity);
  say("parks in bay ", bay);
  return bay;
}

void leave_when_full(Dock &dock, int bay, int capacity)
{
  auto hold = when([&] { return dock.room(bay) == 0; }, dock);
  dock.pull_out(bay, capacity);
  say("is full, pulls out of bay ", bay);
}

//
// The workers. Each function is the whole life of one thread.
//

void machine(Floor &floor, int total)
{
  for (int i = 0; i < total; ++i) {
    busy(0.05, 0.15);
    auto hold = reserve(floor);
    floor.add_box();
    say("built box #", floor.made(), " (", floor.boxes(), " on the floor)");
  }
  {
    auto hold = reserve(floor);
    floor.finish();
  }
  say("finished the order");
}

void forklift(Floor &floor, Door &door, Dock &dock)
{
  while (take_box(floor)) {
    drive_through(door, "out to the dock");
    load_truck(dock);
    drive_through(door, "back to the floor");
  }
  say("floor is clear, parking");
}

void truck(Dock &dock, int trips, int capacity)
{
  for (int trip = 1; trip <= trips; ++trip) {
    int bay = park(dock, capacity);
    leave_when_full(dock, bay, capacity);
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
  Door door;
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

  // Every worker has exited, but the accessors still insist on a hold.
  auto hold = reserve(floor, dock);
  const int shipped = dock.shipped();
  std::cout << "\nordered " << total << ", built " << floor.made()
            << ", shipped " << shipped << ", " << floor.boxes()
            << " left on the floor" << std::endl;
  bool ok = floor.made() == total && shipped == total && floor.boxes() == 0;
  return ok ? 0 : 1;
}
