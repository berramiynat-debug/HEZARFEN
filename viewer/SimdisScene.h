#pragma once
#include "Protocol.h"
#include "simData/MemoryDataStore.h"
#include "simVis/Viewer.h"
#include "simVis/SceneManager.h"
#include "simUtil/HudManager.h"
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
namespace hezarfen {
struct SceneUpdate {
    std::optional<Message> message;
    std::string status;
    bool disconnect{false};
};
class SimdisScene {
public:
    explicit SimdisScene(std::string mode);
    ~SimdisScene();
    void initialize(simVis::Viewer* viewer);
    void pushUpdate(SceneUpdate update);
    void onFrameUpdate();
    void stop();
    void home();
    void writeReport(const std::string& path) const;
private:
    simData::ObjectId getOrCreate(const std::string& key, const std::string& kind);
    void renderState();
    void position(simData::ObjectId id, double lat, double lon, double alt, double heading);
    void style(simData::ObjectId id, const std::string& label, uint32_t color);
    void refreshHud();
    simData::MemoryDataStore dataStore_;
    osg::observer_ptr<simVis::Viewer> viewer_;
    osg::ref_ptr<simVis::SceneManager> scene_;
    std::map<std::string,simData::ObjectId> ids_;
    MissionState state_;
    std::string mode_, sourceStatus_;
    std::mutex mutex_;
    std::deque<SceneUpdate> queue_;
    std::atomic<bool> stopped_{false};
    std::unique_ptr<simUtil::HudManager> hud_;
    osg::ref_ptr<simUtil::HudText> statusText_, rosterText_, eventText_;
    bool positioned_{false};
};
}
