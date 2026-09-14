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
#   bay.lock     a loading bay: which truck is parked there, how much room it has
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
# -- it checks under the lock, lets go, naps briefly, and checks again. Never
# sleep while holding a data lock: nobody else could make the progress you are
# waiting for.
#

import argparse
import random
import sys
import threading
import time

POLL = 0.01  # seconds to nap before checking again

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
        self.boxes = 0  # on the floor right now
        self.made = 0  # built so far
        self.done = False  # the machine has built its last box


class Bay:
    """A loading bay on the dock. Holds at most one truck."""

    def __init__(self, number):
        self.number = number
        self.lock = threading.Lock()
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
    with floor.lock:
        floor.done = True
    log("finished the order")


def forklift(floor, door, bays):
    while take_box(floor):
        drive_through(door, "out to the dock")
        load_truck(bays)
        drive_through(door, "back to the floor")
    log("floor is clear, parking")


def truck(bays, trips, capacity):
    for trip in range(1, trips + 1):
        bay = park(bays, capacity)
        leave_when_full(bay, capacity)
        busy(0.4, 0.8)  # out on delivery
        log(f"delivered load {trip} of {trips}")
    log("done for the day")


#
# The steps. Each one waits as long as it has to, then does its job.
#


def take_box(floor):
    """Pick up a box. Returns False once there will never be another."""
    while True:
        with floor.lock:
            if floor.boxes > 0:
                floor.boxes -= 1
                log(f"picks up a box ({floor.boxes} left on the floor)")
                return True
            if floor.done:
                return False
        time.sleep(POLL)  # floor is empty for now


def drive_through(door, where):
    with door:
        log(f"drives through the door {where}")
        busy(0.05, 0.10)


def load_truck(bays):
    """Put the box we are carrying on any parked truck with room."""
    while True:
        for bay in bays:
            with bay.lock:
                if bay.room > 0:
                    bay.room -= 1
                    log(f"loads {bay.truck} in bay {bay.number} "
                        f"(room for {bay.room} more)")
                    return
        time.sleep(POLL)  # no truck with room yet


def park(bays, capacity):
    """Back into any empty bay."""
    while True:
        for bay in bays:
            with bay.lock:
                if bay.truck is None:
                    bay.truck = threading.current_thread().name
                    bay.room = capacity
                    log(f"parks in bay {bay.number}")
                    return bay
        time.sleep(POLL)  # every bay is taken


def leave_when_full(bay, capacity):
    while True:
        with bay.lock:
            if bay.room == 0:
                bay.truck = None
                bay.shipped += capacity
                log(f"is full, pulls out of bay {bay.number}")
                return
        time.sleep(POLL)  # still loading


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
    bays = [Bay(n) for n in range(1, args.bays + 1)]

    workers = [threading.Thread(target=machine, args=(floor, total),
                                name="machine")]
    for n in range(1, args.forklifts + 1):
        workers.append(threading.Thread(target=forklift,
                                        args=(floor, door, bays),
                                        name=f"forklift{n}"))
    for n in range(1, args.trucks + 1):
        workers.append(threading.Thread(target=truck,
                                        args=(bays, args.trips, args.capacity),
                                        name=f"truck{n}"))

    for worker in workers:
        worker.start()
        log(f"started {worker.name} as OS thread {worker.native_id}")

    for worker in workers:
        worker.join()

    # Every worker has exited, so nobody else can touch the shared state now:
    # reading it without the locks is safe.
    shipped = sum(bay.shipped for bay in bays)
    print(f"\nordered {total}, built {floor.made}, shipped {shipped}, "
          f"{floor.boxes} left on the floor")
    return 0 if floor.made == shipped == total and floor.boxes == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
