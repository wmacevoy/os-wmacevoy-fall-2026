#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <mutex>
#include <random>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>

// Finally is a C++ scope guard that executes a provided function when it goes out of scope, ensuring that resources are released properly even in the case of exceptions or early returns.
// Example usage:
// {     auto release = Finally([](){ /* cleanup code here */ });
//     // code that may throw or return early
// } // cleanup code is automatically called here
// A default-constructed Finally does nothing, and !finally is true: that is
// how requires_all() says it did not get the resources. Moving a Finally hands
// the function to the new one, so it still runs exactly once.
class Finally {
private: std::function<void()> _func;
private: bool _armed = false;
public:  Finally() = default;
public:  Finally(std::function<void()> func) : _func(std::move(func)), _armed(true) {}
public:  bool operator!() const { return !_armed; }
public:  ~Finally() { if (_armed) _func(); }
         Finally(const Finally&) = delete;
         Finally& operator=(const Finally&) = delete;
         Finally(Finally&& other)
             : _func(std::move(other._func)), _armed(std::exchange(other._armed, false)) {}
         Finally& operator=(Finally&&) = delete;
};

// Guarded is a class that provides a mechanism for safely accessing shared resources in a multithreaded environment. It uses a mutex to ensure that only one thread can access the resource at a time, and it allows threads to reserve access to the resource with a timeout to prevent deadlocks. The requires_all function allows multiple resources to be reserved together, ensuring that they are acquired in a consistent order to avoid deadlocks.
// For single resource access, you can use the reserve and release methods directly. For example:
//   if (oven.reserve() == 0) { oven.temperature(350); oven.release(); }
// Reservations nest: a thread that already holds a resource can reserve it
// again, and it is freed when the last reserve is released.
class Guarded {
// Only the owner writes _owner and _reserved_count, but own() reads _owner
// from any thread, so it is atomic. A default thread::id means nobody.
private: std::atomic<std::thread::id> _owner{std::thread::id()};
private: uint32_t _reserved_count = 0;
private: std::timed_mutex _mutex;
public:  bool own() const { return _owner.load() == std::this_thread::get_id(); }
// Each of these returns 0 on success, or an errno value.
public:  int reserve() {
             if (own()) { ++_reserved_count; return 0; }
             try {
                 _mutex.lock();
             } catch (const std::system_error &error) {
                 return error.code().value();
             }
             _owner = std::this_thread::get_id();
             _reserved_count = 1;
             return 0;
         }
public:  int reserve_now() {
             if (own()) { ++_reserved_count; return 0; }
             if (!_mutex.try_lock()) return EBUSY;
             _owner = std::this_thread::get_id();
             _reserved_count = 1;
             return 0;
         }
public:  int reserve(std::chrono::steady_clock::time_point deadline) {
             if (own()) { ++_reserved_count; return 0; }
             if (!_mutex.try_lock_until(deadline)) return ETIMEDOUT;
             _owner = std::this_thread::get_id();
             _reserved_count = 1;
             return 0;
         }
public:  void release() {
             if (own() && --_reserved_count == 0) {
                 _owner = std::thread::id();
                 _mutex.unlock();
             }
         }
protected: void guard() const {
               if (!own()) throw std::runtime_error("Must reserve before accessing guarded resource");
           }
// Only Guarded resources, so that requires_all(timeout, backoff, ...) picks
// the timed version below instead of treating the numbers as resources.
private: template<typename... Resources>
         static constexpr bool all_guarded = (std::is_base_of_v<Guarded, Resources> && ...);
// Each thread has its own generator, so threads never share random state.
private: static std::mt19937 &random() {
             thread_local std::mt19937 engine{std::random_device{}()};
             return engine;
         }
// Every call reserves in the same order, by address, so two calls can't wait
// on each other in a cycle. (std::less, because < on unrelated pointers is
// unspecified.) A thread that already holds some resources and then asks for
// more can still deadlock with another; the timed version recovers from that
// by backing off.
private: template<typename... Resources>
         static std::array<Guarded*, sizeof...(Resources)> in_order(Resources&... resources) {
             std::array<Guarded*, sizeof...(Resources)> required{&resources...};
             std::sort(required.begin(), required.end(), std::less<Guarded*>());
             return required;
         }
public:
    template<typename... Resources>
    static std::enable_if_t<all_guarded<Resources...>, Finally> requires_all(Resources&... resources) {
        auto required = in_order(resources...);
        size_t i = 0;
        while (i < required.size() && required[i]->reserve() == 0) {
            ++i;
        }
        if (i == required.size()) {
            return Finally([required](){
                for (auto* r : required) r->release();
            });
        }
        while (i > 0) {
            required[--i]->release();
        }
        return Finally();
    }

    // Like requires_all, but never waits: an empty Finally if any resource is taken.
    template<typename... Resources>
    static std::enable_if_t<all_guarded<Resources...>, Finally> requires_now(Resources&... resources) {
        auto required = in_order(resources...);
        size_t i = 0;
        while (i < required.size() && required[i]->reserve_now() == 0) {
            ++i;
        }
        if (i == required.size()) {
            return Finally([required](){
                for (auto* r : required) r->release();
            });
        }
        while (i > 0) {
            required[--i]->release();
        }
        return Finally();
    }

    template<typename... Resources>
    static Finally requires_all(uint32_t timeout_us, uint32_t backoff_us, Resources&... resources) {
        auto required = in_order(resources...);
        std::uniform_int_distribution<uint32_t> backoff(1, std::max<uint32_t>(backoff_us, 1));
        for (;;) {
            auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(timeout_us);
            size_t i = 0;
            while (i < required.size() && required[i]->reserve(deadline) == 0) {
                ++i;
            }
            if (i == required.size()) {
                return Finally([required](){
                    for (auto* r : required) r->release();
                });
            }
            while (i > 0) {
                required[--i]->release();
            }
            std::this_thread::sleep_for(std::chrono::microseconds(backoff(random())));
        }
    }
};

class GuardedOven : public Guarded {
private: int _temperature = 0;
public:  int temperature() const { guard(); return _temperature; }
public:  void temperature(int temp) { guard(); _temperature = temp; }
};

class GuardedCookieSheet : public Guarded {
private: int _cookies = 0;
public:  int cookies() const { guard(); return _cookies; }
public:  void cookies(int c) { guard(); _cookies = c; }
};

GuardedOven oven;
GuardedCookieSheet cookieSheet;

// stdout is shared too: print whole lines under a lock so they never mix.
// Nothing else is ever locked while print_lock is held.
std::mutex print_lock;

void say(const char *line) {
    std::scoped_lock hold(print_lock);
    std::cout << line << std::endl;
}

void alice() {
    const int timeout_us = 5'000'000; // 5 seconds in microseconds
    const int backoff_us = 6'000'000; // 6 seconds in microseconds
    auto release = Guarded::requires_all(timeout_us, backoff_us, oven, cookieSheet);
    say("Alice is cooking cookies...");
    cookieSheet.cookies(12);
    oven.temperature(350);
    std::this_thread::sleep_for(std::chrono::seconds(4));
}

void bob() {
    const int timeout_us = 5'000'000; // 5 seconds in microseconds
    const int backoff_us = 6'000'000; // 6 seconds in microseconds
    auto reserved = Guarded::requires_all(timeout_us, backoff_us, oven, cookieSheet);
    if (!reserved) {
        say("Bob failed to acquire resources...");
        return;
    }
    say("Bob is cooking cookies...");
    cookieSheet.cookies(8);
    oven.temperature(400);
    std::this_thread::sleep_for(std::chrono::seconds(6));
}

int main() {
    std::thread alice_thread(alice);
    std::thread bob_thread(bob);
    say("Alice and Bob are cooking cookies...");
    alice_thread.join();
    bob_thread.join();
    return 0;
}
