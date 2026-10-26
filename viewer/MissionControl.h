#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
namespace hezarfen {
struct ControlStatus {
    bool supported{false}, paused{false}, running{false};
    double time{};
    std::string error;
    bool restartSupported{false};
};
// Dedicated loopback HTTP control channel. TCP remains an unchanged observer feed.
class MissionControl {
public:
    using Callback=std::function<void(ControlStatus)>;
    MissionControl(uint16_t port,Callback callback);
    ~MissionControl();
    void start();
    void stop();
    void request(const std::string& action);
    bool busy() const { return busy_; }
private:
    void run();
    uint16_t port_;
    Callback callback_;
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::string action_;
    std::atomic<bool> stop_{false},busy_{false};
};
}
