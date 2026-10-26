#include "Protocol.h"
#include "third_party/json.hpp"
#include <cmath>
#include <set>
#include <sstream>
#include <stdexcept>

namespace hezarfen {
using Json = nlohmann::json;
namespace {
void require(bool condition, const std::string& why) {
    if (!condition) throw std::runtime_error(why);
}
std::string str(const Json& j, const char* key, bool nonempty = true) {
    const auto& value = j.at(key);
    require(value.is_string(), std::string(key) + " must be a string");
    auto result = value.get<std::string>();
    require(!nonempty || !result.empty(), std::string(key) + " must not be empty");
    require(result.size() <= 4096, "string too long");
    // Keep labels/logs readable; escaped JSON remains decoded correctly.
    for (unsigned char c : result) require(c >= 32, "control character in label");
    return result;
}
double number(const Json& j, const char* key, double low, double high) {
    const auto& v = j.at(key);
    require(v.is_number(), std::string(key) + " must be numeric");
    double n = v.get<double>();
    require(std::isfinite(n) && n >= low && n <= high, std::string(key) + " out of range");
    return n;
}
PlatformUpdate platform(const Json& j) {
    PlatformUpdate p;
    p.id = str(j,"id"); p.kind = str(j,"kind"); p.role = str(j,"role");
    require(p.kind == "UAV" || p.kind == "USV" || p.kind == "SHIP", "unknown platform kind");
    p.lat_deg = number(j,"lat_deg",-90,90); p.lon_deg = number(j,"lon_deg",-180,180);
    p.alt_m = number(j,"alt_m",0,1e7);
    require(p.kind == "UAV" ? p.alt_m > 0 : p.alt_m == 0, "surface/air altitude mismatch");
    p.heading_deg = number(j,"heading_deg",0,360);
    require(p.heading_deg < 360, "heading must be <360");
    p.sec = parseSecurityState(str(j,"security_state"));
    return p;
}
ContactUpdate contact(const Json& j) {
    ContactUpdate c;
    c.id = str(j,"id"); c.lat_deg = number(j,"lat_deg",-90,90);
    c.lon_deg = number(j,"lon_deg",-180,180); c.status = str(j,"status"); c.source = str(j,"source");
    require(c.status == "Unverified" || c.status == "Contested" || c.status == "Confirmed", "invalid contact status");
    return c;
}
}
SecurityState parseSecurityState(const std::string& s) {
    if (s == "Trusted") return SecurityState::Trusted;
    if (s == "Watch") return SecurityState::Watch;
    if (s == "Restricted") return SecurityState::Restricted;
    if (s == "Quarantined") return SecurityState::Quarantined;
    if (s == "Revoked") return SecurityState::Revoked;
    throw std::runtime_error("unknown security_state: " + s);
}
std::string securityStateLabel(SecurityState s) {
    switch (s) {
    case SecurityState::Trusted: return "Trusted";
    case SecurityState::Watch: return "Watch";
    case SecurityState::Restricted: return "Restricted";
    case SecurityState::Quarantined: return "Quarantined";
    case SecurityState::Revoked: return "Revoked";
    } return "";
}
uint32_t securityStateColor(SecurityState s) {
    switch (s) {
    case SecurityState::Trusted: return 0x00FF00FF;
    case SecurityState::Watch: return 0xFFFF00FF;
    case SecurityState::Restricted: return 0xFF8000FF;
    case SecurityState::Quarantined: return 0xFF0000FF;
    case SecurityState::Revoked: return 0x808080FF;
    } return 0xFFFFFFFF;
}
Message parseMessage(const std::string& payload) {
    require(!payload.empty() && payload.size() <= MAX_PAYLOAD, "invalid payload size");
    // Bound nesting before building a tree, and reject duplicate object keys.
    std::map<int, std::set<std::string>> keys;
    auto j = Json::parse(payload, [&](int depth, Json::parse_event_t event, Json& value) {
        require(depth <= 32, "JSON nesting limit");
        if (event == Json::parse_event_t::object_start) keys[depth + 1].clear();
        if (event == Json::parse_event_t::key)
            require(keys[depth].insert(value.get<std::string>()).second, "duplicate JSON key");
        return true;
    });
    require(j.is_object(), "envelope must be an object");
    require(j.at("v").is_number_integer() && j.at("v") == 1, "unsupported protocol version");
    require(j.at("seq").is_number_unsigned() && j.at("seq").get<uint64_t>() > 0, "seq must be a positive integer");
    Message m;
    m.seq = j.at("seq").get<uint64_t>(); m.t_sec = number(j,"t_sec",0,1e15);
    m.type = str(j,"type");
    if (j.contains("run_id")) m.run_id = str(j,"run_id");
    const auto& data = j.at("data"); require(data.is_object(), "data must be an object");
    if (m.type == "SNAPSHOT" || m.type == "DELTA") {
        const auto& ps = data.at("platforms"); require(ps.is_array(), "platforms must be an array");
        std::set<std::string> ids;
        for (const auto& p : ps) { auto v = platform(p); require(ids.insert(v.id).second,"duplicate platform id"); m.platforms.push_back(std::move(v)); }
        if (m.type == "SNAPSHOT") {
            const auto& cs = data.at("contacts"); require(cs.is_array(),"contacts must be an array");
            ids.clear();
            for (const auto& c : cs) { auto v = contact(c); require(ids.insert(v.id).second,"duplicate contact id"); m.contacts.push_back(std::move(v)); }
        }
    } else if (m.type == "SECURITY_EVENT") {
        SecurityEvent s;
        s.event_id = str(data,"event_id"); s.subject = str(data,"subject"); s.code = str(data,"code");
        s.result = str(data,"result"); s.scope = str(data,"scope"); s.reason = str(data,"reason");
        require(s.result == "ACCEPTED" || s.result == "REJECTED" || s.result == "APPLIED", "unknown security result");
        if (data.contains("security_state")) s.new_state = parseSecurityState(str(data,"security_state"));
        require(data.at("evidence_ids").is_array(),"evidence_ids must be an array");
        for (const auto& e : data.at("evidence_ids")) { require(e.is_string(),"invalid evidence id"); s.evidence_ids.push_back(e.get<std::string>()); }
        m.security = std::move(s);
    } else if (m.type == "CONTACT_EVENT") m.contacts.push_back(contact(data));
    else if (m.type == "LINK_EVENT") {
        LinkEvent l{str(data,"subject"),str(data,"status"),str(data,"reason",false)};
        require(l.status == "UP" || l.status == "DOWN", "invalid link status"); m.link = std::move(l);
    } else if (m.type == "HEARTBEAT") {
        require(data.at("running").is_boolean(),"running must be boolean"); m.running = data.at("running").get<bool>();
    } else throw std::runtime_error("unknown message type: " + m.type);
    return m;
}
void MissionState::log(const std::string& value) {
    events.push_back(value); if (events.size() > 6) events.pop_front();
}
void MissionState::disconnected(const std::string& reason) {
    stale = true; awaitingSnapshot = true; status = "STALE DATA | " + reason;
}
bool MissionState::apply(const Message& m) {
    const bool snapshot = m.type == "SNAPSHOT";
    if (awaitingSnapshot && !snapshot) return false;
    if (!awaitingSnapshot && m.seq <= seq) return false;
    if (!snapshot && !m.run_id.empty() && !runId.empty() && m.run_id != runId) {
        disconnected("RUN CHANGED - SNAPSHOT REQUIRED"); return false;
    }
    if (snapshot) {
        platforms.clear(); contacts.clear(); running.reset(); ++snapshots;
        if (awaitingSnapshot || m.t_sec < time || m.run_id != runId) events.clear();
        awaitingSnapshot = false; stale = false;
    } else if (m.t_sec < time) {
        disconnected("TIME RESET WITHOUT SNAPSHOT"); return false;
    }
    seq = m.seq; time = m.t_sec;
    if (!m.run_id.empty()) runId = m.run_id;
    for (const auto& p : m.platforms) platforms[p.id].value = p;
    for (const auto& c : m.contacts) contacts[c.id] = c;
    if (m.security) {
        const auto& s = *m.security; ++securityEvents;
        auto it = platforms.find(s.subject);
        if (it != platforms.end()) {
            if (s.new_state) it->second.value.sec = *s.new_state;
            if (s.scope == "MISSION_COMMAND") it->second.lastDecision = s.code + " " + s.result;
        }
        std::ostringstream out; out << "#" << seq << " t=" << time << " " << s.event_id << " " << s.subject << " " << s.code << " " << s.result
            << "\n  " << s.scope << " | " << s.reason << " | evidence:";
        for (const auto& e : s.evidence_ids) out << " " << e;
        log(out.str());
    }
    if (m.link) {
        const auto& l = *m.link;
        auto it = platforms.find(l.subject); if (it != platforms.end()) it->second.link = l.status;
        log("#" + std::to_string(seq) + " " + l.subject + " LINK " + l.status + " | " + l.reason);
    }
    if (m.type == "CONTACT_EVENT") for (const auto& c : m.contacts)
        log("#" + std::to_string(seq) + " " + c.id + " " + c.status + " | source " + c.source);
    if (m.running) running = m.running;
    status = running ? (*running ? "MISSION RUNNING" : "MISSION IDLE / STOPPED") : "SNAPSHOT RECEIVED";
    ++applied; return true;
}
}
