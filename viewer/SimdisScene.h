#pragma once
#include "Protocol.h"
#include "MissionControl.h"
#include "simData/MemoryDataStore.h"
#include "simVis/Viewer.h"
#include "simVis/SceneManager.h"
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
namespace hezarfen {
struct SceneUpdate {
    std::optional<Message> message;
    std::string status;
    bool disconnect{false};
    std::optional<ControlStatus> control;
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
    void drawPanel();
    void enableControls(uint16_t port);
    void setReplayPause(std::atomic<bool>* paused) { replayPaused_=paused; }
private:
    simData::ObjectId getOrCreate(const std::string& key, const std::string& kind);
    void renderState();
    void position(simData::ObjectId id, double lat, double lon, double alt, double heading);
    void style(simData::ObjectId id, const std::string& label, uint32_t color);
    simData::MemoryDataStore dataStore_;
    osg::observer_ptr<simVis::Viewer> viewer_;
    osg::ref_ptr<simVis::SceneManager> scene_;
    std::map<std::string,simData::ObjectId> ids_;
    MissionState state_;
    std::unique_ptr<MissionControl> control_;
    ControlStatus controlStatus_;
    std::atomic<bool>* replayPaused_{};
    std::string mode_, sourceStatus_;
    std::mutex mutex_;
    std::deque<SceneUpdate> queue_;
    std::atomic<bool> stopped_{false};
    enum class CameraAction { None, Mission, Selected, Perspective, Overhead };
    CameraAction cameraAction_{CameraAction::None};
    std::string selected_{"UAV-02"};
    char eventFilter_[128]{};
    bool followEvents_{true};
    std::string shownEvent_;
    uint64_t uiFrames_{};
    bool positioned_{false};
};
}
