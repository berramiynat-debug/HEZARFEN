#include "MissionControl.h"
#include <chrono>
#include <cmath>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
using namespace std::chrono_literals;
int main(int argc,char** argv) {
    try {
        if(argc!=2) throw std::runtime_error("control HTTP port required");
        hezarfen::ControlStatus latest; std::mutex mutex;
        hezarfen::MissionControl control(uint16_t(std::stoi(argv[1])),[&](auto status) {
            std::lock_guard<std::mutex> lock(mutex); latest=std::move(status);
        });
        auto read=[&] { std::lock_guard<std::mutex> lock(mutex); return latest; };
        auto wait=[&](auto condition) {
            const auto end=std::chrono::steady_clock::now()+5s;
            while(std::chrono::steady_clock::now()<end) {
                auto state=read(); if(condition(state)) return state;
                std::this_thread::sleep_for(10ms);
            }
            throw std::runtime_error("control state timeout: "+read().error);
        };
        control.start(); wait([](const auto& s){return s.supported && s.running;});
        control.request("pause"); const auto paused=wait([&](const auto& s){return s.paused && !s.running && !control.busy();});
        std::this_thread::sleep_for(1200ms);
        const auto still=read();
        if(!still.paused || std::abs(still.time-paused.time)>1e-9) throw std::runtime_error("clock advanced while paused");
        control.request("resume"); wait([&](const auto& s){return !s.paused && s.running && s.time>paused.time;});
        control.request("restart");
        wait([&](const auto& s){return s.restartSupported && !s.paused && s.running && s.time<.5 && !control.busy();});
        control.stop();
        std::cout<<"PASS native WinHTTP pause/resume/restart: frozen time, resume, then fresh automatic mission at zero\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<"FAIL "<<e.what()<<"\n";return 1; }
}
