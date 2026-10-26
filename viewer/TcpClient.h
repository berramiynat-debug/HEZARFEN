#pragma once
#include "Replay.h"
#include <atomic>
#include <cstdint>
#include <thread>
namespace hezarfen {
class TcpClient {
public:
    TcpClient(std::string host, uint16_t port, MessageCallback message, StatusCallback status);
    ~TcpClient();
    void start();
    void stop();
private:
    void run();
    std::string host_;
    uint16_t port_;
    MessageCallback message_;
    StatusCallback status_;
    std::atomic<bool> stop_{false};
    std::thread worker_;
};
}
