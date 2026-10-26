#include "EventHandler.h"
#include "simVis/View.h"
#include <osgViewer/CompositeViewer>
namespace hezarfen {
FrameUpdateHandler::FrameUpdateHandler(SimdisScene& scene, simVis::Viewer* viewer, double quitAfter)
    : scene_(scene), viewer_(viewer), quitAfter_(quitAfter) {}
bool FrameUpdateHandler::handle(const osgGA::GUIEventAdapter& e, osgGA::GUIActionAdapter&) {
    if (e.getEventType() == osgGA::GUIEventAdapter::FRAME) {
        if (start_ < 0) start_ = e.getTime();
        scene_.onFrameUpdate();
        if (quitAfter_ > 0 && e.getTime() - start_ >= quitAfter_) viewer_->getMainView()->getViewManager()->getViewer()->setDone(true);
    } else if (e.getEventType() == osgGA::GUIEventAdapter::KEYDOWN) {
        if (e.getKey() == osgGA::GUIEventAdapter::KEY_Home) { scene_.home(); return true; }
        if (e.getKey() == '1' || e.getKey() == '2') { viewer_->getMainView()->enableOverheadMode(e.getKey() == '2'); return true; }
    }
    return false;
}
}
