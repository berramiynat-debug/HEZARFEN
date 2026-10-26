#pragma once
#include "Protocol.h"
#include <atomic>
#include <functional>
#include <string>
namespace hezarfen {
using MessageCallback = std::function<void(const std::string&)>;
using StatusCallback = std::function<void(const std::string&, bool)>;
// Validates the complete file before playback; throws with line number on error.
std::size_t validateReplay(const std::string& path);
void replayFile(const std::string& path, double speed, std::atomic<bool>& stop,
                const MessageCallback& message, const StatusCallback& status);
}
