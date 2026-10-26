#pragma once
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace hezarfen {
constexpr std::size_t MAX_PAYLOAD = 1024 * 1024;
enum class SecurityState { Trusted, Watch, Restricted, Quarantined, Revoked };
SecurityState parseSecurityState(const std::string& value);
std::string securityStateLabel(SecurityState state);
uint32_t securityStateColor(SecurityState state);
struct PlatformUpdate {
    std::string id, kind, role;
    double lat_deg{}, lon_deg{}, alt_m{}, heading_deg{};
    SecurityState sec{SecurityState::Trusted};
};
struct ContactUpdate {
    std::string id;
    double lat_deg{}, lon_deg{};
    std::string status, source;
};
struct SecurityEvent {
    std::string event_id, subject, code, result, scope, reason;
    std::vector<std::string> evidence_ids;
    std::optional<SecurityState> new_state;
};
struct LinkEvent { std::string subject, status, reason; };
struct Message {
    std::string type, run_id;
    uint64_t seq{};
    double t_sec{};
    std::vector<PlatformUpdate> platforms;
    std::vector<ContactUpdate> contacts;
    std::optional<SecurityEvent> security;
    std::optional<LinkEvent> link;
    std::optional<bool> running;
};
// Throws on malformed UTF-8, JSON or schema. No partial updates escape validation.
Message parseMessage(const std::string& payload);
struct PlatformState {
    PlatformUpdate value;
    std::string link{"UP"}, lastDecision;
};
// Pure observer state. No authorization rules or generated security decisions.
class MissionState {
public:
    bool apply(const Message& message);
    void disconnected(const std::string& reason);
    std::map<std::string, PlatformState> platforms;
    std::map<std::string, ContactUpdate> contacts;
    std::deque<std::string> events;
    bool awaitingSnapshot{true}, stale{true};
    std::optional<bool> running;
    uint64_t seq{}, applied{}, securityEvents{}, snapshots{};
    double time{};
    std::string runId, status{"WAITING FOR SNAPSHOT"};
private:
    void log(const std::string& value);
};
}
