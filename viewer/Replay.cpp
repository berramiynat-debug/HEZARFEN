#include "Replay.h"
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <thread>
namespace hezarfen {
namespace {
bool readLine(std::istream& in, std::string& line) {
    line.clear(); char c;
    while (in.get(c)) {
        if (c == '\n') return true;
        if (line.size() >= MAX_PAYLOAD) throw std::runtime_error("JSONL line exceeds 1 MiB");
        line.push_back(c);
    }
    return !line.empty();
}
}
std::size_t validateReplay(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open replay: " + path);
    MissionState check; std::string line; std::size_t count = 0, lineNo = 0;
    while (readLine(in,line)) {
        ++lineNo; if (line.empty() || line == "\r") continue;
        try {
            auto m = parseMessage(line);
            if (count && m.seq <= check.seq) throw std::runtime_error("non-increasing sequence");
            if (!check.apply(m)) throw std::runtime_error("snapshot required / invalid stream order");
            ++count;
        } catch (const std::exception& e) {
            throw std::runtime_error("replay line " + std::to_string(lineNo) + ": " + e.what());
        }
    }
    if (!count) throw std::runtime_error("empty replay");
    return count;
}
void replayFile(const std::string& path, double speed, std::atomic<bool>& stop,
                const MessageCallback& message, const StatusCallback& status,
                const std::atomic<bool>* paused) {
    std::ifstream in(path,std::ios::binary); std::string line;
    auto due = std::chrono::steady_clock::now(); double previous = 0; bool first = true;
    try {
        while (!stop && readLine(in,line)) {
            if (line.empty() || line == "\r") continue;
            auto m = parseMessage(line);
            if (!first) due += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(std::max(0.0,m.t_sec - previous) / speed));
            first = false; previous = m.t_sec;
            while (!stop) {
                const bool frozen=paused && paused->load();
                const auto before=std::chrono::steady_clock::now();
                if(!frozen && before>=due) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                if(frozen) due+=std::chrono::steady_clock::now()-before;
            }
            if (!stop) message(line);
        }
        if (!stop) status("REPLAY COMPLETE | recorded synthetic scenario",false);
    } catch (const std::exception& e) { status(std::string("REPLAY ERROR: ") + e.what(),true); }
}
}
