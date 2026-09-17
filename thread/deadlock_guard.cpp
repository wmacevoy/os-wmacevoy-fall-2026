#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <mutex>
#include <random>
#include <stdexcept>
#include <thread>
#include <utility>

// Runs func when it goes out of scope. Moving a Finally hands func to the new
// one and disarms the old one, so func still runs exactly once.
class Finally {
private: std::function<void()> _func;
private: bool _armed = true;
public:  Finally(std::function<void()> func) : _func(std::move(func)) {}
public:  ~Finally() { if (_armed) _func(); }
         Finally(const Finally&) = delete;
         Finally& operator=(const Finally&) = delete;
         Finally(Finally&& other)
             : _func(std::move(other._func)), _armed(std::exchange(other._armed, false)) {}
         Finally& operator=(Finally&&) = delete;
};

class Guarded {
// Only the thread holding _mutex writes _owner, but own() reads it from any
// thread, so it is atomic. A default-constructed thread::id means nobody.
private: std::atomic<std::thread::id> _owner{std::thread::id()};
private: std::timed_mutex _mutex;
public:  bool own() const { return _owner.load() == std::this_thread::get_id(); }
public:  bool reserve(std::chrono::steady_clock::time_point deadline) {
             // Locking a std::timed_mutex you already hold is undefined.
             if (own()) throw std::logic_error("Already reserved by this thread");
             if (!_mutex.try_lock_until(deadline)) return false;
             _owner = std::this_thread::get_id();
             return true;
         }
public:  void release() {
             if (own()) {
                 _owner = std::thread::id();
                 _mutex.unlock();
             }
         }
protected: void guard() const {
               if (!own()) throw std::runtime_error("Must reserve before accessing guarded resource");
           }
// Each thread has its own generator, so threads never share random state.
private: static std::mt19937 &random() {
             thread_local std::mt19937 engine{std::random_device{}()};
             return engine;
         }
public:
    template<typename... Resources>
    static Finally requires_all(uint32_t timeout_ms, uint32_t backoff_ms, Resources&... resources) {
        std::array<Guarded*, sizeof...(Resources)> arr{&resources...};
        // Everyone reserves in the same order, by address, so no two threads
        // can wait on each other in a cycle. std::less, because < on
        // unrelated pointers is unspecified. Duplicates are reserved once.
        std::sort(arr.begin(), arr.end(), std::less<Guarded*>());
        size_t n = std::unique(arr.begin(), arr.end()) - arr.begin();
        // Check before reserving anything, so a throw never leaves one held.
        for (size_t k = 0; k < n; ++k) {
            if (arr[k]->own()) throw std::logic_error("Already reserved by this thread");
        }
        std::uniform_int_distribution<uint32_t> backoff(1, std::max<uint32_t>(backoff_ms, 1));
        for (;;) {
            auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
            size_t i = 0;
            while (i < n && arr[i]->reserve(deadline)) {
                ++i;
            }
            if (i == n) {
                return Finally([arr, n](){
                    for (size_t k = 0; k < n; ++k) arr[k]->release();
                });
            }
            while (i > 0) {
                arr[--i]->release();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(backoff(random())));
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
    const int timeout_ms = 5000;
    const int backoff_ms = 6000;
    auto release = Guarded::requires_all(timeout_ms, backoff_ms, oven, cookieSheet);
    say("Alice is cooking cookies...");
    cookieSheet.cookies(12);
    oven.temperature(350);
    std::this_thread::sleep_for(std::chrono::seconds(4));
}

void bob() {
    const int timeout_ms = 5000;
    const int backoff_ms = 6000;
    auto release = Guarded::requires_all(timeout_ms, backoff_ms, oven, cookieSheet);
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
