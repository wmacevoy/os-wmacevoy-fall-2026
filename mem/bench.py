#!/usr/bin/env python3
#
# bench.py -- time table and sequence operations on a Python dict, list and
# collections.deque.
#
# Same workload as bench.cpp and bench.js, so the numbers line up.
# The first five ops use the structure as a table from key to value:
#
#   keys   = a shuffled permutation of 0..n-1 (same order in every language)
#   insert : table[key] = 2*key            for each key
#   lookup : sum += table[key]             for each key   (all hits)
#   miss   : count += (n+key in table)     for each key   (all misses)
#   iterate: sum += value                  for every entry
#   erase  : remove key                    for each key
#
# The last two start from empty and add n entries, each one at an end:
#
#   append-last : sequence: append.          dict: keys 0, 1, ..., n-1
#   append-first: sequence: insert at front. dict: keys n-1, ..., 1, 0
#
# Besides time, each op reports "resizes": how many steps changed the
# container's own storage, seen as a change in sys.getsizeof. A deque is a
# list of fixed-size blocks with no single block to move, so it shows "-".
# Python gives no count of individual heap calls, so allocs/frees print "-".
#
# usage: ./bench.py [--reps R] [n ...]
#

import sys
import time
from collections import deque
from sys import getsizeof

OPS = ["insert", "lookup", "miss", "iterate", "erase", "append-last", "append-first"]

# Ops that cost O(n) per step (so n^2 in all) are skipped above this size:
# list append-first shifts the whole list over, and indexing a deque walks
# its blocks from the nearer end.
QUADRATIC_MAX = 100_000


def shuffled(n):
    """Fisher-Yates shuffle of 0..n-1 driven by xorshift32 (same as bench.cpp)."""
    keys = list(range(n))
    x = 2026
    for i in range(n - 1, 0, -1):
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        j = x % (i + 1)
        keys[i], keys[j] = keys[j], keys[i]
    return keys


def seconds(f):
    t0 = time.perf_counter()
    f()
    return time.perf_counter() - t0


def check(ok, what):
    if not ok:
        sys.exit(f"check failed: {what}")


def resizes(table, steps, step):
    """Run step(x) for each x in steps, counting changes in the table's footprint.

    Checking after every step costs time, so this is a separate, untimed run.
    """
    count = 0
    last = getsizeof(table)
    for x in steps:
        step(x)
        now = getsizeof(table)
        count += now != last
        last = now
    return count


# Each op is its own function so the loop runs with fast local variables,
# the way you would actually write it.

def dict_pass(keys):
    n = len(keys)
    table = {}
    out = {}

    def insert():
        for k in keys:
            table[k] = 2 * k

    def lookup():
        s = 0
        for k in keys:
            s += table[k]
        out["sum"] = s

    def miss():
        c = 0
        for k in keys:
            c += (n + k) in table
        out["count"] = c

    def iterate():
        s = 0
        for v in table.values():
            s += v
        out["sum"] = s

    def erase():
        for k in keys:
            del table[k]

    def append_last():
        d = {}
        for k in range(n):
            d[k] = 2 * k
        out["len"] = len(d)

    def append_first():
        d = {}
        for k in range(n - 1, -1, -1):
            d[k] = 2 * k
        out["len"] = len(d)

    t = [seconds(insert), seconds(lookup)]
    check(out["sum"] == n * (n - 1), "lookup sum")
    t.append(seconds(miss))
    check(out["count"] == 0, "miss count")
    t.append(seconds(iterate))
    check(out["sum"] == n * (n - 1), "iterate sum")
    t.append(seconds(erase))
    check(not table, "erase empty")
    for f in (append_last, append_first):
        t.append(seconds(f))
        check(out["len"] == n, "append size")
    return t


def dict_resizes(keys):
    n = len(keys)
    table = {}
    grow = resizes(table, keys, lambda k: table.__setitem__(k, 2 * k))
    shrink = resizes(table, keys, table.__delitem__)
    last = {}
    first = {}
    return [grow, 0, 0, 0, shrink,
            resizes(last, range(n), lambda k: last.__setitem__(k, 2 * k)),
            resizes(first, range(n - 1, -1, -1), lambda k: first.__setitem__(k, 2 * k))]


# A list or deque is a "table" whose keys are exactly 0..n-1: the key *is* the
# index. It grows on demand as keys arrive, and a slot holding EMPTY has no key.
# Erase empties the slot and pops empty slots off the end; both give memory
# back on their own as they shrink.
EMPTY = -1


def seq_set(table, k):
    if k >= len(table):
        table.extend([EMPTY] * (k + 1 - len(table)))
    table[k] = 2 * k


def seq_del(table, k):
    table[k] = EMPTY
    while table and table[-1] == EMPTY:
        table.pop()


def seq_pass(keys, make, push_first, table_ops=True, first_op=True):
    """make() builds an empty sequence; push_first(seq, v) puts v at the front.

    table_ops/first_op False skip those ops (reported as None).
    """
    n = len(keys)
    table = make()
    out = {}

    # insert and erase are seq_set/seq_del written inline, since a function
    # call per key would cost more than the work being timed.
    def insert():
        for k in keys:
            if k >= len(table):
                table.extend([EMPTY] * (k + 1 - len(table)))
            table[k] = 2 * k

    def lookup():
        s = 0
        for k in keys:
            s += table[k]
        out["sum"] = s

    def miss():
        c = 0
        size = len(table)
        for k in keys:
            q = n + k
            c += q < size and table[q] != EMPTY
        out["count"] = c

    def iterate():
        s = 0
        for v in table:
            if v != EMPTY:
                s += v
        out["sum"] = s

    def erase():
        for k in keys:
            table[k] = EMPTY
            while table and table[-1] == EMPTY:
                table.pop()

    def append_last():
        s = make()
        push = s.append
        for k in range(n):
            push(2 * k)
        out["len"] = len(s)

    def append_first():
        s = make()
        for k in range(n - 1, -1, -1):
            push_first(s, 2 * k)
        out["len"] = len(s)

    t = [None] * 5
    if table_ops:
        t = [seconds(insert), seconds(lookup)]
        check(out["sum"] == n * (n - 1), "lookup sum")
        t.append(seconds(miss))
        check(out["count"] == 0, "miss count")
        t.append(seconds(iterate))
        check(out["sum"] == n * (n - 1), "iterate sum")
        t.append(seconds(erase))
        check(not table, "erase empty")
    t.append(seconds(append_last))
    check(out["len"] == n, "append size")
    t.append(None)
    if first_op:
        t[6] = seconds(append_first)
        check(out["len"] == n, "append size")
    return t


def list_pass(keys):
    return seq_pass(keys, list, lambda s, v: s.insert(0, v),
                    first_op=len(keys) <= QUADRATIC_MAX)


def list_resizes(keys):
    n = len(keys)
    table = []
    grow = resizes(table, keys, lambda k: seq_set(table, k))
    shrink = resizes(table, keys, lambda k: seq_del(table, k))
    last = []
    first = []
    return [grow, 0, 0, 0, shrink,
            resizes(last, range(n), lambda k: last.append(2 * k)),
            resizes(first, range(n - 1, -1, -1), lambda k: first.insert(0, 2 * k))
            if n <= QUADRATIC_MAX else None]


def deque_pass(keys):
    return seq_pass(keys, deque, deque.appendleft,
                    table_ops=len(keys) <= QUADRATIC_MAX)


def report(name, n, reps, run, count):
    best = [None] * len(OPS)
    for _ in range(reps):
        for i, t in enumerate(run()):
            if t is not None and (best[i] is None or t < best[i]):
                best[i] = t
    counts = count() if count else [None] * len(OPS)
    for op, t, r in zip(OPS, best, counts):
        ns = "-" if t is None else f"{1e9 * t / n:.1f}"
        r = "-" if r is None else r
        print(f"{'python':<8} {name:<22} {n:>10} {op:<12} {ns:>10} {r:>8} {'-':>8} {'-':>8}",
              flush=True)


def main(argv):
    reps = 3
    sizes = []
    args = iter(argv[1:])
    for a in args:
        if a == "--reps":
            reps = int(next(args))
        else:
            sizes.append(int(a))
    if not sizes:
        sizes = [1_000, 10_000, 100_000, 1_000_000]

    print(f"{'lang':<8} {'structure':<22} {'n':>10} {'op':<12} {'ns/op':>10}"
          f" {'resizes':>8} {'allocs':>8} {'frees':>8}")
    for n in sizes:
        keys = shuffled(n)
        report("dict", n, reps, lambda: dict_pass(keys), lambda: dict_resizes(keys))
        report("list", n, reps, lambda: list_pass(keys), lambda: list_resizes(keys))
        report("collections.deque", n, reps, lambda: deque_pass(keys), None)


if __name__ == "__main__":
    main(sys.argv)
