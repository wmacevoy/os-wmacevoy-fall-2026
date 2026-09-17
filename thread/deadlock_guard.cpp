#include <thread>
#include <time.h>
#include <functional>
#include <chrono>
#include <array>
#include <iostream>
#include <cstdlib>
#include <unistd.h>

class Finally {
private: std::function<void()> _func;
public:  Finally(std::function<void()> func) : _func(std::move(func)) {}
public:  ~Finally() { _func(); }
         Finally(const Finally&) = delete;
         Finally& operator=(const Finally&) = delete;
         Finally(Finally&&) = default;
         Finally& operator=(Finally&&) = default;
};

class Guarded {
private: pthread_t _owner = 0;
private: pthread_mutex_t _mutex = PTHREAD_MUTEX_INITIALIZER;
public:  bool own() const { return _owner == pthread_self(); }
public:  bool reserve(const std::chrono::system_clock::time_point &deadline) {
             auto secs = std::chrono::time_point_cast<std::chrono::seconds>(deadline);
             auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - secs);
             struct timespec ts = { .tv_sec = secs.time_since_epoch().count(),
                                    .tv_nsec = ns.count() };
             if (pthread_mutex_timedlock(&_mutex, &ts) == 0) {
                 _owner = pthread_self();
                 return true;
             }
             return false;
         }
public:  void release() {
             if (own()) {
                 _owner = 0;
                 pthread_mutex_unlock(&_mutex);
             }
         }
protected: void guard() const {
               if (!own()) throw std::runtime_error("Must reserve before accessing guarded resource");
           }
public:
    template<typename... Resources>
    static Finally requires_all(uint32_t timeout_ms, uint32_t backoff_ms, Resources&... resources) {
        std::array<Guarded*, sizeof...(Resources)> arr{&resources...};
        for (;;) {
            auto deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(timeout_ms);
            size_t i = 0;
            while (i < arr.size() && arr[i]->reserve(deadline)) {
                ++i;
            }
            if (i == arr.size()) {
                return Finally([arr](){
                    for (auto* r : arr) r->release();
                });
            }
            while (i > 0) {
                arr[--i]->release();
            }
            usleep(1000 * (rand() % backoff_ms + 1));
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
    const int timeout_ms = 5000;
    const int backoff_ms = 6000;
    auto release = Guarded::requires_all(timeout_ms, backoff_ms, oven, cookieSheet);
    std::cout << "Alice is cooking cookies..." << std::endl;
    cookieSheet.cookies(12);
    oven.temperature(350);
    sleep(4);
}

void bob() {
    const int timeout_ms = 5000;
    const int backoff_ms = 6000;
    auto release = Guarded::requires_all(timeout_ms, backoff_ms, oven, cookieSheet);
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