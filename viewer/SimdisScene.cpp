#include "SimdisScene.h"
#include "Coordinates.h"
#include "third_party/json.hpp"
#include "simVis/Scenario.h"
#include "simVis/Platform.h"
#include "simVis/View.h"
#include "simVis/NavigationModes.h"
#include "simUtil/ExampleResources.h"
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <set>
#include <stdexcept>
namespace hezarfen {
SimdisScene::SimdisScene(std::string mode) : mode_(std::move(mode)) {}
SimdisScene::~SimdisScene() {
    stop();
    if (scene_.valid()) scene_->getScenario()->unbind(&dataStore_);
}
void SimdisScene::initialize(simVis::Viewer* viewer) {
    viewer_ = viewer; scene_ = viewer->getSceneManager();
    scene_->getScenario()->bind(&dataStore_);
    viewer->setNavigationMode(simVis::NAVMODE_ROTATEPAN);
    home();
}
void SimdisScene::pushUpdate(SceneUpdate update) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) return;
    // Fail explicitly and resynchronize, never silently discard security events.
    if (queue_.size() >= 1024 && update.message) throw std::runtime_error("render queue overflow; reconnect snapshot required");
    queue_.push_back(std::move(update));
}
void SimdisScene::enableControls(uint16_t port) {
    control_=std::make_unique<MissionControl>(port,[this](ControlStatus status) {
        SceneUpdate update; update.control=std::move(status); pushUpdate(std::move(update));
    });
    control_->start();
}
void SimdisScene::stop() { stopped_ = true; if(control_) control_->stop(); }
void SimdisScene::onFrameUpdate() {
    std::deque<SceneUpdate> updates;
    { std::lock_guard<std::mutex> lock(mutex_); updates.swap(queue_); }
    for (const auto& update : updates) {
        if(update.control) controlStatus_=*update.control;
        if (!update.status.empty()) {
            sourceStatus_ = update.status;
            if (update.disconnect) state_.disconnected(update.status);
        }
        if (update.message && state_.apply(*update.message)) {
            renderState();
            if (update.message->security || update.message->link || update.message->type == "CONTACT_EVENT")
                std::cout << "[HEZARFEN] " << state_.events.back() << std::endl;
            if (!positioned_ && !state_.platforms.empty()) { home(); positioned_ = true; }
        }
    }
    switch (cameraAction_) {
    case CameraAction::Mission: home(); break;
    case CameraAction::Selected: {
        auto it = state_.platforms.find(selected_);
        if(it != state_.platforms.end()) {
            const auto& p = it->second.value;
            viewer_->getMainView()->lookAt(p.lat_deg,p.lon_deg,p.alt_m,0,-45,1800);
        }
        break;
    }
    case CameraAction::Perspective: viewer_->getMainView()->enableOverheadMode(false); break;
    case CameraAction::Overhead: viewer_->getMainView()->enableOverheadMode(true); break;
    default: break;
    }
    cameraAction_=CameraAction::None;
}
simData::ObjectId SimdisScene::getOrCreate(const std::string& key, const std::string& kind) {
    auto it = ids_.find(key); if (it != ids_.end()) return it->second;
    simData::DataStore::Transaction tx;
    auto* props = dataStore_.addPlatform(&tx); const auto id = props->id(); tx.complete(&props);
    ids_[key] = id;
    simData::DataStore::Transaction px;
    auto* prefs = dataStore_.mutable_platformPrefs(id,&px);
    prefs->mutable_commonprefs()->set_draw(true);
    auto* label = prefs->mutable_commonprefs()->mutable_labelprefs();
    label->set_draw(true); label->set_overlayfontpointsize(13);
    label->set_textoutline(simData::TextOutline::TO_THICK); label->set_outlinecolor(0x000000FF);
    const bool left = key == "P:UAV-01" || key == "P:USV-01" || kind == "CONTACT";
    label->set_alignment(left ? simData::TextAlignment::ALIGN_RIGHT_CENTER : simData::TextAlignment::ALIGN_LEFT_CENTER);
    label->set_offsetx(left ? -24 : 24); label->set_offsety(8);
    prefs->set_icon(kind == "UAV" ? EXAMPLE_AIRPLANE_ICON : EXAMPLE_SHIP_ICON);
    prefs->set_scale(key == "P:CMD-01" ? 4.5 : 3.0);
    prefs->set_lighted(false);
    prefs->set_dynamicscale(true);
    prefs->set_drawcirclehilight(true);
    prefs->set_circlehilightshape(kind == "CONTACT" ? simData::CircleHilightShape::CH_DIAMOND : simData::CircleHilightShape::CH_CIRCLE);
    prefs->set_circlehilightsize(.9);
    prefs->set_lifespanmode(simData::LifespanMode::LIFE_EXTEND_SINGLE_POINT);
    px.complete(&prefs);
    std::cout << "[HEZARFEN] SDK platform " << key << " id=" << id << std::endl;
    return id;
}
void SimdisScene::position(simData::ObjectId id, double lat, double lon, double alt, double heading) {
    const auto ecef = toEcef(lat,lon,alt,heading);
    simData::DataStore::Transaction tx;
    auto* update = dataStore_.addPlatformUpdate(id,&tx);
    // -1 is the SDK's static TSPI time. Replace the current sample on receipt;
    // the last known location remains visible when paused/disconnected.
    update->set_time(-1.0);
    update->setPosition(ecef.position()); update->setOrientation(ecef.orientation());
    tx.complete(&update);
}
void SimdisScene::style(simData::ObjectId id, const std::string& text, uint32_t color) {
    simData::DataStore::Transaction tx;
    auto* prefs = dataStore_.mutable_platformPrefs(id,&tx);
    prefs->mutable_commonprefs()->set_name(text);
    prefs->mutable_commonprefs()->set_useoverridecolor(true);
    prefs->mutable_commonprefs()->set_overridecolor(color);
    prefs->mutable_commonprefs()->mutable_labelprefs()->set_color(color);
    prefs->set_circlehilightcolor(color); tx.complete(&prefs);
}
void SimdisScene::renderState() {
    std::set<std::string> active;
    for (const auto& [key,p] : state_.platforms) {
        const auto full = "P:" + key; active.insert(full);
        auto id = getOrCreate(full,p.value.kind);
        position(id,p.value.lat_deg,p.value.lon_deg,p.value.alt_m,p.value.heading_deg);
        std::string label = key + " [" + securityStateLabel(p.value.sec) + "]";
        if (p.link == "DOWN") label += "\nLINK DOWN";
        if (!p.lastDecision.empty() && key == "UAV-02") label += "\n" + p.lastDecision.substr(p.lastDecision.rfind(' ') + 1);
        style(id,label,securityStateColor(p.value.sec));
    }
    for (const auto& [key,c] : state_.contacts) {
        const auto full = "C:" + key; active.insert(full); auto id = getOrCreate(full,"CONTACT");
        position(id,c.lat_deg,c.lon_deg,0,0);
        style(id,key + " [" + c.status + "]\nSource: " + c.source,0xFFFFFFFF);
    }
    for (auto it = ids_.begin(); it != ids_.end();) {
        if (!active.count(it->first)) { dataStore_.removeEntity(it->second); it = ids_.erase(it); }
        else ++it;
    }
    dataStore_.update(state_.time);
}
void SimdisScene::home() {
    if (!viewer_.valid()) return;
    double lat = 40.567, lon = 27.506;
    if (!state_.platforms.empty()) {
        lat = lon = 0;
        for (const auto& [id,p] : state_.platforms) { lat += p.value.lat_deg; lon += p.value.lon_deg; }
        lat /= state_.platforms.size(); lon /= state_.platforms.size();
    }
    viewer_->getMainView()->lookAt(lat,lon - .004,0,0,-55,4800);
}
void SimdisScene::writeReport(const std::string& path) const {
    nlohmann::json j = {{"project","HEZARFEN"},{"ui","Dear ImGui"},{"ui_frames",uiFrames_},{"mode",mode_},{"seq",state_.seq},{"t_sec",state_.time},
        {"stale",state_.stale},{"paused",replayPaused_ ? replayPaused_->load() : controlStatus_.paused},
        {"pause_controls_available",replayPaused_!=nullptr || controlStatus_.supported},{"control_error",controlStatus_.error},
        {"awaiting_snapshot",state_.awaitingSnapshot},{"applied",state_.applied},
        {"security_events",state_.securityEvents},{"snapshots",state_.snapshots},{"source_status",sourceStatus_},
        {"platforms",nlohmann::json::array()},{"contacts",nlohmann::json::array()},{"events",state_.events}};
    for (const auto& [id,p] : state_.platforms) {
        auto ecef = toEcef(p.value.lat_deg,p.value.lon_deg,p.value.alt_m,p.value.heading_deg);
        j["platforms"].push_back({{"id",id},{"sdk_id",ids_.at("P:" + id)},{"lat_deg",p.value.lat_deg},
            {"lon_deg",p.value.lon_deg},{"alt_m",p.value.alt_m},{"security_state",securityStateLabel(p.value.sec)},
            {"link",p.link},{"ecef_m",{ecef.x(),ecef.y(),ecef.z()}}});
        auto* node = scene_->getScenario()->find<simVis::PlatformNode>(ids_.at("P:" + id));
        j["platforms"].back()["sdk_active"] = node && node->isActive();
        if (node && node->update()) j["platforms"].back()["sdk_position_m"] = {node->update()->x(),node->update()->y(),node->update()->z()};
    }
    for (const auto& [id,c] : state_.contacts) j["contacts"].push_back({{"id",id},{"status",c.status},{"source",c.source}});
    std::ofstream out(path); if (!out) throw std::runtime_error("cannot write report: " + path);
    out << j.dump(2) << '\n';
}
}
