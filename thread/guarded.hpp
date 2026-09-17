//
// guarded.hpp -- shared resources you wait on by condition, not by signal.
//
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Warren MacEvoy -- https://github.com/wmacevoy/guarded-threads
//
//   auto hold = when([&] { return !q.empty(); }, q);   // wait, then hold q
//   int x = q.pop();                                   // until end of scope
//
// A thread names everything it needs up front. when() locks all of it (in
// address order, so there is no deadlock), and checks the condition. If the
// condition is false, it releases everything and sleeps until another thread
// changes one of those resources, then tries again. Nobody calls notify.
//
// A resource is a class derived from Resource. Its const accessors start
// with const_guard(), and every other accessor starts with guard(). Both
// throw if the calling thread does not hold the resource. guard() also marks
// the resource as changed, and releasing a changed resource wakes the
// threads waiting on it.
//
// Waiting on any of several resources, pthreads style, would mean waiting on
// several condition variables at once, and there is no such call. So it is
// turned around: each thread has one condition variable of its own, and each
// resource keeps a list of the threads waiting on it (the way Go's select
// works).
//
// retry(body, resources...) runs a block that may call balk() anywhere --
// even inside helpers that use when() themselves. A balk releases
// everything, waits for a change, and runs the block again. There is no
// undo, so a block must balk before it changes anything; balk() checks.
//
// This is a conditional critical region (Brinch Hansen and Hoare, 1972),
// and balk() is the lock-based cousin of retry in software transactional
// memory (Harris et al., 2005).
//

#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace guarded {

// Thrown by balk(), caught by the enclosing retry().
struct Balked {};

class Resource;

namespace detail {

using Resources = std::vector<Resource *>;

// What a thread sleeps on while it waits. A shared_ptr, because a resource
// may still list a thread that has since woken up, or even exited.
struct Waiter {
  std::mutex mutex;
  std::condition_variable wake;
  bool woken = false;

  void poke()
  {
    {
      std::scoped_lock lock(mutex);
      woken = true;
    }
    wake.notify_one();
  }
};
using WaiterPtr = std::shared_ptr<Waiter>;

struct ThreadState {
  Resources held;          // what the outermost guard reserved, if any
  bool retryable = false;  // the outermost guard is a retry()
  WaiterPtr waiter = std::make_shared<Waiter>();
};
inline thread_local ThreadState self;

struct Access;

}  // namespace detail

// Derive from this, and start every accessor with const_guard() or guard().
class Resource {
public:
  Resource() = default;
  Resource(const Resource &) = delete;
  Resource &operator=(const Resource &) = delete;

  // True if the calling thread holds this resource.
  bool own() const { return owner.load() == std::this_thread::get_id(); }

protected:
  // For const accessors.
  void const_guard() const
  {
    if (!own()) throw std::logic_error("hold a resource before using it");
  }

  // For every other accessor, even one that happens not to change anything.
  void guard()
  {
    const_guard();
    changed = true;  // so releasing it wakes the threads waiting on it
  }

private:
  friend struct detail::Access;

  // Only the thread holding mutex touches the rest.
  std::mutex mutex;
  std::atomic<std::thread::id> owner{std::thread::id()};
  bool changed = false;                    // since it was reserved
  std::vector<detail::WaiterPtr> waiters;  // threads waiting on this
};

namespace detail {

struct Access {
  // Lock everything, in the (sorted) order given. A thread that was waiting
  // on these is awake now, so it takes itself off their lists.
  static void acquire(const Resources &rs)
  {
    const WaiterPtr &me = self.waiter;
    for (Resource *r : rs) {
      r->mutex.lock();
      r->owner = std::this_thread::get_id();
      auto &ws = r->waiters;
      ws.erase(std::remove(ws.begin(), ws.end(), me), ws.end());
    }
  }

  static bool any_changed(const Resources &rs)
  {
    return std::any_of(rs.begin(), rs.end(),
                       [](Resource *r) { return r->changed; });
  }

  // Unlock everything. Whoever was waiting on a changed resource gets woken,
  // after the locks are gone, so they can take them.
  static void release(const Resources &rs)
  {
    std::vector<WaiterPtr> wake;
    for (auto it = rs.rbegin(); it != rs.rend(); ++it) {
      Resource *r = *it;
      if (r->changed) {
        r->changed = false;
        wake.insert(wake.end(), r->waiters.begin(), r->waiters.end());
        r->waiters.clear();
      }
      r->owner = std::thread::id();
      r->mutex.unlock();
    }
    for (const WaiterPtr &w : wake) {
      w->poke();
    }
  }

  // Nothing was changed. Get on every resource's list while still holding
  // them all, so no change can slip by unseen, then release them and sleep
  // until a thread that changes one of them wakes us.
  static void park(const Resources &rs)
  {
    WaiterPtr me = self.waiter;
    {
      std::scoped_lock lock(me->mutex);
      me->woken = false;
    }
    for (Resource *r : rs) {
      r->waiters.push_back(me);
    }
    release(rs);
    std::unique_lock lock(me->mutex);
    me->wake.wait(lock, [&] { return me->woken; });
  }
};

// Address order, without duplicates: the one order every thread locks in.
inline Resources sorted(Resources rs)
{
  std::sort(rs.begin(), rs.end(), std::less<Resource *>());
  rs.erase(std::unique(rs.begin(), rs.end()), rs.end());
  return rs;
}

// A nested guard can only name what the outermost guard already holds:
// taking something new while holding other things could deadlock.
inline void require_held(const Resources &rs)
{
  for (Resource *r : rs) {
    if (!r->own()) {
      throw std::logic_error("a nested guard can only name resources "
                             "the outermost guard holds");
    }
  }
}

struct Adopt {};

}  // namespace detail

// Holds resources until it goes out of scope. An empty Hold (from a nested
// guard) holds nothing.
class Hold {
public:
  Hold() = default;
  Hold(Hold &&other) : rs(std::exchange(other.rs, {})) {}
  Hold &operator=(Hold &&) = delete;
  ~Hold() { end(); }

  // For when() and retry(): take over resources this thread just locked.
  Hold(detail::Adopt, detail::Resources locked, bool retryable)
    : rs(std::move(locked))
  {
    detail::self.held = rs;
    detail::self.retryable = retryable;
  }

  // For retry(): release everything and sleep until something changes.
  void park()
  {
    forget();
    detail::Access::park(std::exchange(rs, {}));
  }

private:
  detail::Resources rs;

  void forget()
  {
    detail::self.held.clear();
    detail::self.retryable = false;
  }
  void end()
  {
    if (!rs.empty()) {
      forget();
      detail::Access::release(std::exchange(rs, {}));
    }
  }
};

// Give up on this attempt: the enclosing retry() releases everything, waits
// for a change, and starts over. Changes can't be undone, so balk first.
[[noreturn]] inline void balk()
{
  const detail::ThreadState &self = detail::self;
  if (self.held.empty()) {
    throw std::logic_error("balk() outside a guard");
  }
  if (!self.retryable) {
    throw std::logic_error("balk() needs an enclosing retry()");
  }
  if (detail::Access::any_changed(self.held)) {
    throw std::logic_error("balk() after a change");
  }
  throw Balked{};
}

// Hold the resources for the rest of the scope, once ready() is true.
// Nested inside another guard, a false ready() balks instead of waiting.
template <typename Ready, typename... Rs>
[[nodiscard]] Hold when(Ready ready, Rs &...resources)
{
  static_assert(sizeof...(Rs) > 0, "name at least one resource");
  detail::Resources rs = detail::sorted({&resources...});
  if (!detail::self.held.empty()) {
    detail::require_held(rs);
    if (!ready()) balk();
    return Hold();
  }
  for (;;) {
    detail::Access::acquire(rs);
    Hold hold(detail::Adopt{}, rs, false);
    if (ready()) {
      return hold;
    }
    if (detail::Access::any_changed(rs)) {
      throw std::logic_error("a when() condition must not change anything");
    }
    hold.park();
  }
}

// Hold the resources for the rest of the scope.
template <typename... Rs>
[[nodiscard]] Hold reserve(Rs &...resources)
{
  return when([] { return true; }, resources...);
}

// Run body holding the resources; each time it balks, wait for a change
// and run it again. Returns what body returns. Nested inside another
// guard, just runs body, and a balk goes to the outer retry().
template <typename Body, typename... Rs>
decltype(auto) retry(Body body, Rs &...resources)
{
  static_assert(sizeof...(Rs) > 0, "name at least one resource");
  detail::Resources rs = detail::sorted({&resources...});
  if (!detail::self.held.empty()) {
    detail::require_held(rs);
    return body();
  }
  for (;;) {
    detail::Access::acquire(rs);
    Hold hold(detail::Adopt{}, rs, true);
    try {
      return body();
    } catch (const Balked &) {
      hold.park();
    }
  }
}

}  // namespace guarded
