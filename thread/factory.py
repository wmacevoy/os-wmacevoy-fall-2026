#!/usr/bin/env python3
#
# factory.py -- a factory simulated with one real thread per worker.
#
# A machine builds boxes, forklifts carry them through the dock door, and
# delivery trucks wait in a loading bay until they are full, then drive off.
#
# Threads are what keep this simple. Each worker is a plain loop that reads
# like its job description -- see forklift() and truck(). Without threads we
# would need an event loop and a state machine per forklift remembering "has a
# box, halfway through the door, waiting for a truck". With threads, that
# bookkeeping is just each thread's program counter and local variables, and
# the OS scheduler does the interleaving.
#
# threading.Thread is a real OS thread (a pthread on macOS and Linux). The GIL
# lets only one run Python bytecode at a time, but sleeping and waiting on a
# lock release it, so the workers genuinely overlap.
#
# Anything touched by more than one thread is guarded by a mutex (threading.Lock):
#
#   floor.lock   the pile of finished boxes on the factory floor
#   door         the dock door: one forklift fits through at a time
#   dock.lock    the loading bays: which truck is parked where, how much room
#   print_lock   stdout, so log lines never mix
#
# Most locks guard data and are held for an instant. The door guards a
# physical thing, so a forklift holds it the whole time it is driving through.
#
# A thread never holds two of the first three locks at once, and nothing is
# ever locked while print_lock is held, so there is no cycle of waiting threads
# and no deadlock (compare philosophers.c).
#
# When a thread has to wait for something -- a box, an empty bay, a full truck
# -- it waits on a condition variable (threading.Condition). wait() unlocks
# the mutex, sleeps until another thread calls notify(), and locks the mutex
# again before it returns. Every wait has the same shape:
#
#   with lock:
#       while not <what we need>:
#           condition.wait()
#       <use it>
#
# Always a while, never an if: by the time a woken thread has the mutex back,
# another thread may have taken the box first. (pthread_cond_wait may even
# return when nobody signaled.)
#
# Whoever changes the data calls notify() to wake one waiter, or notify_all()
# to wake them all, while still holding the lock:
#
#   floor.changed        a box was built, or the machine finished
#   dock.has_room        a truck parked with room to fill
#   dock.has_empty_bay   a truck pulled out
#   bay.full             the truck in this bay just got its last box
#
# A condition variable belongs to one mutex, and everything its while loop
# tests must be guarded by that mutex. A forklift waits for room on *any*
# truck, so a single lock guards all of the bays, and the dock's and the bays'
# condition variables all share it.
#
# These map directly onto pthread_cond_wait(), pthread_cond_signal() and
# pthread_cond_broadcast().
#

import argparse
import random
import sys
import threading
import time

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


class Floor:
    """Finished boxes waiting to be picked up."""

    def __init__(self):
        self.lock = threading.Lock()
        self.changed = threading.Condition(self.lock)
        self.boxes = 0  # on the floor right now
        self.made = 0  # built so far
        self.done = False  # the machine has built its last box


class Dock:
    """The loading bays. One lock guards all of them."""

    def __init__(self, bays):
        self.lock = threading.Lock()
        self.has_room = threading.Condition(self.lock)
        self.has_empty_bay = threading.Condition(self.lock)
        self.bays = [Bay(n, self.lock) for n in range(1, bays + 1)]


class Bay:
    """A loading bay on the dock. Holds at most one truck."""

    def __init__(self, number, lock):
        self.number = number
        self.full = threading.Condition(lock)  # lock is the dock's
        self.truck = None  # name of the parked truck, or None
        self.room = 0  # boxes the parked truck can still take
        self.shipped = 0  # boxes that have left from this bay


#
# The workers. Each function is the whole life of one thread.
#


def machine(floor, total):
    for _ in range(total):
        busy(0.05, 0.15)
        with floor.lock:
            floor.boxes += 1
            floor.made += 1
            log(f"built box #{floor.made} ({floor.boxes} on the floor)")
            floor.changed.notify()  # one new box, so wake one forklift
    with floor.lock:
        floor.done = True
        floor.changed.notify_all()  # every waiting forklift can go park
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
# The steps. Each one waits as long as it has to, then does its job.
#


def take_box(floor):
    """Pick up a box. Returns False once there will never be another."""
    with floor.lock:
        while floor.boxes == 0 and not floor.done:
            floor.changed.wait()
        if floor.boxes == 0:
            return False
        floor.boxes -= 1
        log(f"picks up a box ({floor.boxes} left on the floor)")
        return True


def drive_through(door, where):
    with door:
        log(f"drives through the door {where}")
        busy(0.05, 0.10)


def load_truck(dock):
    """Put the box we are carrying on any parked truck with room."""
    with dock.lock:
        while not any(bay.room > 0 for bay in dock.bays):
            dock.has_room.wait()
        bay = next(bay for bay in dock.bays if bay.room > 0)
        bay.room -= 1
        log(f"loads {bay.truck} in bay {bay.number} "
            f"(room for {bay.room} more)")
        if bay.room == 0:
            bay.full.notify()  # wake the truck parked here


def park(dock, capacity):
    """Back into any empty bay."""
    with dock.lock:
        while not any(bay.truck is None for bay in dock.bays):
            dock.has_empty_bay.wait()
        bay = next(bay for bay in dock.bays if bay.truck is None)
        bay.truck = threading.current_thread().name
        bay.room = capacity
        log(f"parks in bay {bay.number}")
        dock.has_room.notify(capacity)  # wake up to that many forklifts
        return bay


def leave_when_full(dock, bay, capacity):
    with dock.lock:
        while bay.room > 0:
            bay.full.wait()
        bay.truck = None
        bay.shipped += capacity
        log(f"is full, pulls out of bay {bay.number}")
        dock.has_empty_bay.notify()  # one bay opened, so wake one truck


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
    door = threading.Lock()
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

    # Every worker has exited, so nobody else can touch the shared state now:
    # reading it without the locks is safe.
    shipped = sum(bay.shipped for bay in dock.bays)
    print(f"\nordered {total}, built {floor.made}, shipped {shipped}, "
          f"{floor.boxes} left on the floor")
    return 0 if floor.made == shipped == total and floor.boxes == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
