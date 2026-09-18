#!/usr/bin/env python3
#
# factory_guarded.py -- factory.py rebuilt on guarded.py.
#
# The same factory, with the same options and the same story. What changes
# is how a worker waits. factory.py gives the floor and the dock
# threading.Condition objects, loops on wait(), and has every change notify
# the right one. Here there are no conditions and no notify calls. A step
# names the resources it needs and the condition it waits for:
#
#   with when(lambda: floor.boxes() > 0 or floor.done(), floor):
#
# when() holds the floor for the with-block, once the condition is true.
# Until then it holds nothing and sleeps, and any worker that changes the
# floor wakes it to check again (see guarded.py).
#
# Floor and Dock are plain data with methods. A method that only reads
# starts with self.const_guard() and any other with self.guard(). Both raise
# GuardError if the calling thread does not hold the resource, so a
# forgotten lock is an error rather than a race.
#
#   floor        the pile of finished boxes on the factory floor
#   door         the dock door: one forklift fits through at a time
#   dock         the loading bays: which truck is parked where, how much room
#   print_lock   stdout, so log lines never mix
#
# Each step holds one resource at a time, so there is no deadlock. (A step
# that needed two would name both in one when(), which takes them in a
# fixed order.)
#
# stdout stays a plain threading.Lock. log() is called while holding other
# resources, and a guard may not take a new resource while it holds others.
# That is safe here because print_lock is a leaf: nothing else is ever
# locked while it is held.
#

import argparse
import random
import sys
import threading
import time

from guarded import Resource, reserve, when

START = time.monotonic()
print_lock = threading.Lock()


def log(message):
    with print_lock:
        stamp = time.monotonic() - START
        name = threading.current_thread().name
        print(f"{stamp:5.2f}s  {name:<10} {message}", flush=True)


def busy(lo, hi):
    """Spend a random number of seconds doing the job."""
    time.sleep(random.uniform(lo, hi))


class Floor(Resource):
    """Finished boxes waiting to be picked up."""

    def __init__(self):
        super().__init__()
        self._boxes = 0     # on the floor right now
        self._made = 0      # built so far
        self._done = False  # the machine has built its last box

    def boxes(self):
        self.const_guard()
        return self._boxes

    def made(self):
        self.const_guard()
        return self._made

    def done(self):
        self.const_guard()
        return self._done

    def add_box(self):
        self.guard()
        self._boxes += 1
        self._made += 1

    def remove_box(self):
        self.guard()
        self._boxes -= 1

    def finish(self):
        self.guard()
        self._done = True


class Door(Resource):
    """The dock door has no data. Holding it is the whole point."""


class Bay:
    """A loading bay on the dock. Holds at most one truck."""

    def __init__(self):
        self.truck = None  # name of the parked truck, or None
        self.room = 0      # boxes the parked truck can still take
        self.shipped = 0   # boxes that have left from this bay


class Dock(Resource):
    """The loading bays, numbered from 1."""

    def __init__(self, count):
        super().__init__()
        self._bays = [Bay() for _ in range(count)]

    def empty_bay(self):
        """The first bay with no truck, or 0 if every bay is taken."""
        self.const_guard()
        return self._find(lambda bay: bay.truck is None)

    def bay_with_room(self):
        """The first bay whose truck has room, or 0 if there is none."""
        self.const_guard()
        return self._find(lambda bay: bay.room > 0)

    def truck(self, number):
        self.const_guard()
        return self._bays[number - 1].truck

    def room(self, number):
        self.const_guard()
        return self._bays[number - 1].room

    def shipped(self):
        self.const_guard()
        return sum(bay.shipped for bay in self._bays)

    def park(self, number, truck, capacity):
        self.guard()
        bay = self._bays[number - 1]
        bay.truck = truck
        bay.room = capacity

    def load(self, number):
        self.guard()
        self._bays[number - 1].room -= 1

    def pull_out(self, number, capacity):
        self.guard()
        bay = self._bays[number - 1]
        bay.truck = None
        bay.shipped += capacity

    def _find(self, test):
        for number, bay in enumerate(self._bays, 1):
            if test(bay):
                return number
        return 0


#
# The workers. Each function is the whole life of one thread.
#


def machine(floor, total):
    for _ in range(total):
        busy(0.05, 0.15)
        with reserve(floor):
            floor.add_box()
            log(f"built box #{floor.made()} ({floor.boxes()} on the floor)")
    with reserve(floor):
        floor.finish()
    log("finished the order")


def forklift(floor, door, dock):
    while take_box(floor):
        drive_through(door, "out to the dock")
        load_truck(dock)
        drive_through(door, "back to the floor")
    log("floor is clear, parking")


def truck(dock, trips, capacity):
    for trip in range(1, trips + 1):
        bay = park(dock, capacity)
        leave_when_full(dock, bay, capacity)
        busy(0.4, 0.8)  # out on delivery
        log(f"delivered load {trip} of {trips}")
    log("done for the day")


#
# The steps. Each one says what it waits for, then does its job.
#


def take_box(floor):
    """Pick up a box. Returns False once there will never be another."""
    with when(lambda: floor.boxes() > 0 or floor.done(), floor):
        if floor.boxes() == 0:
            return False
        floor.remove_box()
        log(f"picks up a box ({floor.boxes()} left on the floor)")
        return True


def drive_through(door, where):
    with reserve(door):
        log(f"drives through the door {where}")
        busy(0.05, 0.10)


def load_truck(dock):
    """Put the box we are carrying on any parked truck with room."""
    with when(lambda: dock.bay_with_room() != 0, dock):
        bay = dock.bay_with_room()
        dock.load(bay)
        log(f"loads {dock.truck(bay)} in bay {bay} "
            f"(room for {dock.room(bay)} more)")


def park(dock, capacity):
    """Back into any empty bay. Returns the bay's number."""
    with when(lambda: dock.empty_bay() != 0, dock):
        bay = dock.empty_bay()
        dock.park(bay, threading.current_thread().name, capacity)
        log(f"parks in bay {bay}")
        return bay


def leave_when_full(dock, bay, capacity):
    with when(lambda: dock.room(bay) == 0, dock):
        dock.pull_out(bay, capacity)
        log(f"is full, pulls out of bay {bay}")


def main():
    parser = argparse.ArgumentParser(
        description="Simulate a factory with one thread per worker.")
    parser.add_argument("--forklifts", type=int, default=3)
    parser.add_argument("--trucks", type=int, default=3)
    parser.add_argument("--bays", type=int, default=2)
    parser.add_argument("--trips", type=int, default=2,
                        help="deliveries each truck makes")
    parser.add_argument("--capacity", type=int, default=3,
                        help="boxes in a full truck")
    args = parser.parse_args()
    if min(vars(args).values()) < 1:
        parser.error("every count must be at least 1")

    # Build exactly as many boxes as the trucks will carry away, so every
    # worker finishes and the program ends on its own.
    total = args.trucks * args.trips * args.capacity

    floor = Floor()
    door = Door()
    dock = Dock(args.bays)

    workers = [threading.Thread(target=machine, args=(floor, total),
                                name="machine")]
    for n in range(1, args.forklifts + 1):
        workers.append(threading.Thread(target=forklift,
                                        args=(floor, door, dock),
                                        name=f"forklift{n}"))
    for n in range(1, args.trucks + 1):
        workers.append(threading.Thread(target=truck,
                                        args=(dock, args.trips, args.capacity),
                                        name=f"truck{n}"))

    for worker in workers:
        worker.start()
        log(f"started {worker.name} as OS thread {worker.native_id}")

    for worker in workers:
        worker.join()

    # Every worker has exited, but the methods still insist on a hold.
    with reserve(floor, dock):
        made, left, shipped = floor.made(), floor.boxes(), dock.shipped()
    print(f"\nordered {total}, built {made}, shipped {shipped}, "
          f"{left} left on the floor")
    return 0 if made == shipped == total and left == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
