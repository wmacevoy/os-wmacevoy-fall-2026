#include <thread>
#include <time.h>
#include <functional>
#include <chrono>
#include <array>
#include <iostream>
#include <cstdlib>
#include <unistd.h>

// Finally is a C++ scope guard that executes a provided function when it goes out of scope, ensuring that resources are released properly even in the case of exceptions or early returns.
// Example usage:
// {     auto release = Finally([](){ /* cleanup code here */ });
//     // code that may throw or return early
// } // cleanup code is automatically called here
class Finally {
public:  static const std::function<void()> _nothing;
private: std::function<void()> _func;
public:  Finally() : _func(_nothing) {}
public: bool operator!() const { return _func == _nothing; }
public:  Finally(std::function<void()> func) : _func(std::move(func)) {}
public:  ~Finally() { _func(); }
         Finally(const Finally&) = delete;
         Finally& operator=(const Finally&) = delete;
         Finally(Finally&&) = default;
         Finally& operator=(Finally&&) = default;
};

// Guarded is a class that provides a mechanism for safely accessing shared resources in a multithreaded environment. It uses a mutex to ensure that only one thread can access the resource at a time, and it allows threads to reserve access to the resource with a timeout to prevent deadlocks. The requires_all function allows multiple resources to be reserved together, ensuring that they are acquired in a consistent order to avoid deadlocks.
// For single resource access, you can use the rereserve and release methods directly. For example:
class Guarded {
private: pthread_t _owner = 0;
private: uint32_t _reserved_count = 0;
private: pthread_mutex_t _mutex = PTHREAD_MUTEX_INITIALIZER;
public:  bool own() const { return _owner == pthread_self(); }
public:  int reserve() {
    if (own()) { ++_reserved_count; return 0; }
    int status = pthread_mutex_lock(&_mutex);
    if (status == 0) {
        _owner = pthread_self();
        _reserved_count = 1;
    }
    return status;
}
public:  int reserve_now() {
    if (own()) { ++_reserved_count; return 0; }
    int status = pthread_mutex_trylock(&_mutex);
    if (status == 0) {
        _owner = pthread_self();
        _reserved_count = 1;
        return 0;
    }
    return status;
}
public:  int reserve(const std::chrono::system_clock::time_point &deadline) {
    if (own()) { ++_reserved_count; return 0; }
    auto secs = std::chrono::time_point_cast<std::chrono::seconds>(deadline);
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - secs);
    struct timespec ts = { .tv_sec = secs.time_since_epoch().count(),
                        .tv_nsec = ns.count() };
    status = pthread_mutex_timedlock(&_mutex, &ts);
    if (status == 0) {
        _owner = pthread_self();
        _reserved_count = 1;
        return 0;
    }
    return status;
}

public:  void release() {
             if (own() && --_reserved_count == 0) {
                 _owner = 0;
                 pthread_mutex_unlock(&_mutex);
             }
         }
protected: void guard() const {
               if (!own()) throw std::runtime_error("Must reserve before accessing guarded resource");
           }
public:
    template<typename... Resources>
    static Finally requires_all(Resources&... resources) {
        std::array<Guarded*, sizeof...(Resources)> required{&resources...};
        std::sort(required.begin(), required.end(), [](Guarded* a, Guarded* b) { return a < b; }); // Sort by address to prevent deadlock
        for (;;) {
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
    }

    static Finally requires_now(Resources&... resources) {
        std::array<Guarded*, sizeof...(Resources)> required{&resources...};
        std::sort(required.begin(), required.end(), [](Guarded* a, Guarded* b) { return a < b; }); // Sort by address to prevent deadlock
        for (;;) {
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
    }

    template<typename... Resources>
    static Finally requires_all(uint32_t timeout_us, uint32_t backoff_us, Resources&... resources) {
        std::array<Guarded*, sizeof...(Resources)> required{&resources...};
        std::sort(required.begin(), required.end(), [](Guarded* a, Guarded* b) { return (void*)a < (void*)b; }); // Sort by address to prevent deadlock
        for (;;) {
            auto deadline = std::chrono::system_clock::now() + std::chrono::microseconds(timeout_us);
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
            usleep((rand() % backoff_us + 1));
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
   
void alice() {
    const int timeout_us = 5'000'000; // 5 seconds in microseconds
    const int backoff_us = 6'000'000; // 6 seconds in microseconds
    auto release = Guarded::requires_all(timeout_us, backoff_us, oven, cookieSheet);
    std::cout << "Alice is cooking cookies..." << std::endl;
    cookieSheet.cookies(12);
    oven.temperature(350);
    sleep(4);
}

void bob() {
    const int timeout_us = 5'000'000; // 5 seconds in microseconds
    const int backoff_us = 6'000'000; // 6 seconds in microseconds
    auto reserved = Guarded::requires_all(timeout_us, backoff_us, oven, cookieSheet);
    if (!reserved) {
        std::cout << "Bob failed to acquire resources..." << std::endl;
        return;
    }
    std::cout << "Bob is cooking cookies..." << std::endl;
    cookieSheet.cookies(8);
    oven.temperature(400);
    sleep(6);
}

int main() {
    std::thread alice_thread(alice);
    std::thread bob_thread(bob);
    std::cout << "Alice and Bob are cooking cookies..." << std::endl;
    alice_thread.join();
    bob_thread.join();
    return 0;
}