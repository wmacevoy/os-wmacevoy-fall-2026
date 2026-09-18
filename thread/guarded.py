"""
guarded.py -- shared resources you wait on by condition, not by signal.

SPDX-License-Identifier: MIT
Copyright (c) 2026 Warren MacEvoy -- https://github.com/wmacevoy/guarded-threads

    with when(lambda: not q.empty(), q):    # wait, then hold q
        item = q.pop()                       # until the block ends

A thread names everything it needs up front. when() locks all of it (in a
fixed order, so there is no deadlock) and checks the condition. If the
condition is false, it releases everything and sleeps until another thread
changes one of those resources, then tries again. Nobody calls notify.

A resource is a class derived from Resource. Methods that only read start
with self.const_guard(); every other method starts with self.guard(). Both
raise GuardError if the calling thread does not hold the resource. guard()
also marks the resource as changed, and releasing a changed resource wakes
the threads waiting on it.

retry(body, *resources) calls body() while holding the resources. body may
call balk() anywhere -- even inside helpers that use when() themselves. A
balk releases everything, waits for a change, and calls body() again. There
is no undo, so body must balk before it changes anything; balk() checks.

This is the Python twin of include/guarded.hpp. The README explains how it
works and where it stops working.
"""

import threading

__all__ = ["Resource", "GuardError", "Balked",
           "when", "reserve", "retry", "balk"]


class GuardError(RuntimeError):
    """A guarded resource was used the wrong way."""


class Balked(BaseException):
    """Raised by balk(), caught by the enclosing retry().

    A BaseException, like KeyboardInterrupt, so that an `except Exception:`
    inside a retry() body doesn't swallow it.
    """


class _Waiter:
    """What a thread sleeps on while it waits for a resource to change."""

    def __init__(self):
        self.lock = threading.Lock()
        self.wake = threading.Condition(self.lock)
        self.woken = False

    def poke(self):
        with self.lock:
            self.woken = True
            self.wake.notify()


class _ThreadState(threading.local):
    """Each thread gets its own copy of these."""

    def __init__(self):
        self.held = []          # what the outermost guard reserved, if any
        self.retryable = False  # the outermost guard is a retry()
        self.waiter = _Waiter()


_self = _ThreadState()


class Resource:
    """Derive from this, and start every method with const_guard() or guard().

    A subclass that defines __init__ must call super().__init__().
    """

    def __init__(self):
        # Only the thread holding _guard_lock touches the rest.
        self._guard_lock = threading.Lock()
        self._guard_owner = None     # get_ident() of the holding thread
        self._guard_changed = False  # since it was reserved
        self._guard_waiters = []     # threads waiting on this

    def own(self):
        """True if the calling thread holds this resource."""
        return self._guard_owner == threading.get_ident()

    def const_guard(self):
        """For methods that only read."""
        if not self.own():
            raise GuardError("hold a resource before using it")

    def guard(self):
        """For every other method, even one that happens not to change anything."""
        self.const_guard()
        self._guard_changed = True  # so releasing it wakes whoever waits on it


#
# The machinery. rs is always a list of resources in lock order.
#

def _sorted(resources):
    """Resources in the one order every thread locks them in, once each."""
    unique = {id(r): r for r in resources}
    return [unique[key] for key in sorted(unique)]


def _acquire(rs):
    """Lock everything. A thread that was waiting on these is awake now, so
    it takes itself off their lists."""
    me = _self.waiter
    ident = threading.get_ident()
    for r in rs:
        r._guard_lock.acquire()
        r._guard_owner = ident
        r._guard_waiters = [w for w in r._guard_waiters if w is not me]


def _any_changed(rs):
    return any(r._guard_changed for r in rs)


def _release(rs):
    """Unlock everything. Whoever was waiting on a changed resource gets
    woken, after the locks are gone, so they can take them."""
    wake = []
    for r in reversed(rs):
        if r._guard_changed:
            r._guard_changed = False
            wake.extend(r._guard_waiters)
            r._guard_waiters = []
        r._guard_owner = None
        r._guard_lock.release()
    for waiter in wake:
        waiter.poke()


def _park(rs):
    """Nothing was changed. Get on every resource's list while still holding
    them all, so no change can slip by unseen, then release them and sleep
    until a thread that changes one of them wakes us."""
    me = _self.waiter
    with me.lock:
        me.woken = False
    for r in rs:
        r._guard_waiters.append(me)
    _release(rs)
    with me.lock:
        while not me.woken:
            me.wake.wait()


def _adopt(rs, retryable):
    _self.held = rs
    _self.retryable = retryable


def _forget():
    _self.held = []
    _self.retryable = False


def _require_held(rs):
    """A nested guard can only name what the outermost guard already holds:
    taking something new while holding other things could deadlock."""
    for r in rs:
        if not r.own():
            raise GuardError("a nested guard can only name resources "
                             "the outermost guard holds")


#
# The interface.
#

def balk():
    """Give up on this attempt: the enclosing retry() releases everything,
    waits for a change, and starts over. Changes can't be undone, so balk
    first."""
    if not _self.held:
        raise GuardError("balk() outside a guard")
    if not _self.retryable:
        raise GuardError("balk() needs an enclosing retry()")
    if _any_changed(_self.held):
        raise GuardError("balk() after a change")
    raise Balked()


class when:
    """Hold the resources for a with-block, once ready() is true.

    Nested inside another guard, a false ready() balks instead of waiting.
    """

    def __init__(self, ready, *resources):
        if not resources:
            raise GuardError("name at least one resource")
        self._ready = ready
        self._rs = _sorted(resources)
        self._holding = False

    def __enter__(self):
        rs = self._rs
        if _self.held:
            _require_held(rs)
            if not self._ready():
                balk()
            return self
        while True:
            _acquire(rs)
            _adopt(rs, False)
            try:
                ready = self._ready()
            except BaseException:
                _forget()
                _release(rs)
                raise
            if ready:
                self._holding = True
                return self
            _forget()
            if _any_changed(rs):
                _release(rs)
                raise GuardError("a when() condition must not change anything")
            _park(rs)

    def __exit__(self, *exc_info):
        if self._holding:
            self._holding = False
            _forget()
            _release(self._rs)
        return False


def reserve(*resources):
    """Hold the resources for a with-block."""
    return when(lambda: True, *resources)


def retry(body, *resources):
    """Call body() holding the resources; each time it balks, wait for a
    change and call it again. Returns what body() returns.

    Nested inside another guard, just calls body(), and a balk goes to the
    outer retry().
    """
    if not resources:
        raise GuardError("name at least one resource")
    rs = _sorted(resources)
    if _self.held:
        _require_held(rs)
        return body()
    while True:
        _acquire(rs)
        _adopt(rs, True)
        try:
            result = body()
        except Balked:
            _forget()
            _park(rs)
            continue
        except BaseException:
            _forget()
            _release(rs)
            raise
        _forget()
        _release(rs)
        return result
