#include "TcpClient.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <array>
#include <chrono>
#include <stdexcept>
#include <utility>
namespace hezarfen {
namespace {
using Clock = std::chrono::steady_clock;
struct Socket {
    SOCKET value{INVALID_SOCKET};
    ~Socket() { if (value != INVALID_SOCKET) closesocket(value); }
};
bool ready(SOCKET s, bool write) {
    fd_set set; FD_ZERO(&set); FD_SET(s,&set);
    fd_set error; FD_ZERO(&error); FD_SET(s,&error);
    timeval timeout{0,100000};
    int result = select(0,write ? nullptr : &set,write ? &set : nullptr,&error,&timeout);
    if (result == SOCKET_ERROR || FD_ISSET(s,&error)) throw std::runtime_error("socket error");
    return result > 0;
}
bool readExact(SOCKET s, char* out, int count, const std::atomic<bool>& stop) {
    int offset = 0; auto deadline = Clock::now() + std::chrono::seconds(5);
    while (!stop && offset < count) {
        if (Clock::now() >= deadline) throw std::runtime_error("5 s receive timeout");
        if (!ready(s,false)) continue;
        int n = recv(s,out + offset,count - offset,0);
        if (n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) continue;
        if (n <= 0) return false;
        offset += n;
    }
    return offset == count;
}
}
TcpClient::TcpClient(std::string host, uint16_t port, MessageCallback message, StatusCallback status)
    : host_(std::move(host)), port_(port), message_(std::move(message)), status_(std::move(status)) {}
TcpClient::~TcpClient() { stop(); }
void TcpClient::start() {
    if (worker_.joinable()) return;
    stop_ = false; worker_ = std::thread(&TcpClient::run,this);
}
void TcpClient::stop() {
    stop_ = true;
    // Socket ownership stays on the worker; no truncated 64-bit handles or close races.
    if (worker_.joinable()) worker_.join();
}
void TcpClient::run() {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2,2),&wsa) != 0) { status_("Winsock startup failed",true); return; }
    while (!stop_) {
        try {
            Socket s; s.value = socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
            if (s.value == INVALID_SOCKET) throw std::runtime_error("socket creation failed");
            u_long nonblocking = 1;
            if (ioctlsocket(s.value,FIONBIO,&nonblocking)) throw std::runtime_error("nonblocking setup failed");
            sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(port_);
            if (inet_pton(AF_INET,host_.c_str(),&address.sin_addr) != 1) throw std::runtime_error("invalid IPv4 address");
            status_("CONNECTING " + host_ + ":" + std::to_string(port_),true);
            const int result = connect(s.value,reinterpret_cast<sockaddr*>(&address),sizeof(address));
            if (result == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) throw std::runtime_error("connection refused");
            const auto deadline = Clock::now() + std::chrono::seconds(2);
            bool connected = result == 0;
            while (!stop_ && !connected && Clock::now() < deadline) connected = ready(s.value,true);
            if (stop_) break;
            int error = 0, length = sizeof(error);
            if (!connected || getsockopt(s.value,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&error),&length) || error)
                throw std::runtime_error("connection failed");
            status_("TCP CONNECTED | WAITING FOR SNAPSHOT",true);
            // Backend sends a snapshot automatically. The channel is one-way: send no request bytes.
            while (!stop_) {
                std::array<unsigned char,4> header{};
                if (!readExact(s.value,reinterpret_cast<char*>(header.data()),4,stop_)) break;
                const uint32_t size = (uint32_t(header[0]) << 24) | (uint32_t(header[1]) << 16) |
                    (uint32_t(header[2]) << 8) | uint32_t(header[3]);
                if (!size || size > MAX_PAYLOAD) throw std::runtime_error("invalid frame length: " + std::to_string(size));
                std::string payload(size,'\0');
                if (!readExact(s.value,payload.data(),static_cast<int>(size),stop_)) break;
                message_(payload);
            }
            if (!stop_) status_("TCP DISCONNECTED",true);
        } catch (const std::exception& e) { if (!stop_) status_(std::string("TCP: ") + e.what(),true); }
        for (int i = 0; i < 20 && !stop_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    WSACleanup();
}
}
