#pragma once
#include "SimdisScene.h"
#include <osgGA/GUIEventHandler>
namespace hezarfen {
class JsonParser {
public:
    explicit JsonParser(SimdisScene& scene) : scene_(scene) {}
    void operator()(const std::string& json) { scene_.pushUpdate({parseMessage(json),{},false}); }
private: SimdisScene& scene_;
};
class FrameUpdateHandler : public osgGA::GUIEventHandler {
public:
    FrameUpdateHandler(SimdisScene& scene, simVis::Viewer* viewer, double quitAfter);
    bool handle(const osgGA::GUIEventAdapter& event, osgGA::GUIActionAdapter&) override;
private:
    SimdisScene& scene_;
    osg::observer_ptr<simVis::Viewer> viewer_;
    double quitAfter_, start_{-1};
};
}
