#include "EventHandler.h"
#include "ImGuiLayer.h"
#include "TcpClient.h"
#include "Replay.h"
#include "simCore/Common/Version.h"
#include "simCore/Common/HighPerformanceGraphics.h"
#include "simUtil/ExampleResources.h"
#include "simVis/View.h"
#include <osg/ArgumentParser>
#include <osg/Image>
#include <osgDB/WriteFile>
#include <osgViewer/CompositeViewer>
#include <osgViewer/GraphicsWindow>
#include <osg/GL>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <thread>

namespace {
class Capture : public osg::Camera::DrawCallback {
public:
    Capture(std::string path, double after) : path_(std::move(path)),after_(after),start_(std::chrono::steady_clock::now()) {}
    void operator()(osg::RenderInfo& info) const override {
        if (done_ || std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count() < after_) return;
        auto* viewport = info.getCurrentCamera()->getViewport();
        GLint bounds[4]; glGetIntegerv(GL_VIEWPORT,bounds);
        osg::ref_ptr<osg::Image> shot = new osg::Image;
        shot->readPixels(0,0,viewport ? int(viewport->width()) : bounds[2],viewport ? int(viewport->height()) : bounds[3],GL_RGBA,GL_UNSIGNED_BYTE);
        done_ = true;
        std::cout << "[HEZARFEN] OpenGL renderer: " << glGetString(GL_RENDERER) << "; version " << glGetString(GL_VERSION) << std::endl;
        if (!osgDB::writeImageFile(*shot,path_)) std::cerr << "[HEZARFEN] CAPTURE FAILED: " << path_ << std::endl;
        else std::cout << "[HEZARFEN] Actual framebuffer captured: " << path_ << std::endl;
    }
private:
    std::string path_; double after_;
    std::chrono::steady_clock::time_point start_;
    mutable bool done_{false};
};
double positive(const std::string& value) {
    std::size_t used = 0; double n = std::stod(value,&used);
    if (used != value.size() || !std::isfinite(n) || n <= 0) throw std::runtime_error("expected positive number");
    return n;
}
}
int main(int argc, char** argv) {
    try {
        std::string replay, capture, report, host = "127.0.0.1";
        double speed = 1, quitAfter = 0, captureAt = 5;
        uint16_t port = 5556, controlPort = 8000;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            auto next = [&]() -> std::string { if (++i >= argc) throw std::runtime_error("missing value for " + arg); return argv[i]; };
            if (arg == "--replay") replay = next();
            else if (arg == "--speed") speed = positive(next());
            else if (arg == "--host") host = next();
            else if (arg == "--port") { const double n = positive(next()); if (n > 65535 || std::floor(n) != n) throw std::runtime_error("invalid port"); port = uint16_t(n); }
            else if (arg == "--control-port") { const double n=positive(next()); if(n>65535 || std::floor(n)!=n) throw std::runtime_error("invalid control port"); controlPort=uint16_t(n); }
            else if (arg == "--quit-after") quitAfter = positive(next());
            else if (arg == "--capture") capture = next();
            else if (arg == "--capture-at") captureAt = positive(next());
            else if (arg == "--report") report = next();
            else if (arg == "--help") {
                std::cout << "HEZARFEN | SIMDIS Mission Visualization\n"
                    << "Live (default): --host 127.0.0.1 --port 5556\n"
                    << "Local pause controls: --control-port 8000 (viewer.presentation_backend)\n"
                    << "Offline: --replay <events.jsonl> [--speed <factor>]\n"
                    << "Validation: --quit-after <seconds> --capture <image.png> --capture-at <seconds> --report <report.json>\n";
                return 0;
            } else throw std::runtime_error("unknown argument: " + arg);
        }
        if (replay.empty() && speed != 1) throw std::runtime_error("--speed requires --replay");
        if (!replay.empty()) std::cout << "[HEZARFEN] Validated " << hezarfen::validateReplay(replay) << " recorded messages" << std::endl;
        simCore::checkVersionThrow();
        const auto data = std::filesystem::path(simExamples::getSampleDataPath());
        for (const auto& relative : {"terrain/lowResEarth.mbtiles","models/USSV/ussv.ive","models/aqm-37c/aqm-37c.ive"})
            if (!std::filesystem::exists(data / relative)) throw std::runtime_error("missing SDK data: " + (data / relative).string() + "; set SIMDIS_SDK_FILE_PATH");
        simExamples::configureSearchPaths();
        // Keep HEZARFEN arguments out of OSG's argument processing.
        osg::ref_ptr<simVis::Viewer> viewer = new simVis::Viewer(simVis::Viewer::WINDOWED,40,30,1200,650);
        viewer->setMap(simExamples::createDefaultExampleMap());
        simExamples::addDefaultSkyNode(viewer.get());
        auto* view = viewer->getMainView();
        view->getViewManager()->getViewer()->setThreadingModel(osgViewer::ViewerBase::SingleThreaded);
        hezarfen::SimdisScene scene(replay.empty() ? "LIVE TCP" : "REPLAY");
        scene.initialize(viewer.get());
        view->addEventHandler(new hezarfen::FrameUpdateHandler(scene,viewer.get(),quitAfter));
        osg::ref_ptr<hezarfen::ImGuiLayer> gui = new hezarfen::ImGuiLayer([&]() { scene.drawPanel(); });
        view->getEventHandlers().emplace_front(gui.get());
        gui->install(view->getOrCreateHUD());
        if (!capture.empty()) view->getOrCreateHUD()->addFinalDrawCallback(new Capture(capture,captureAt));
        view->getViewManager()->getViewer()->realize();
        osgViewer::ViewerBase::Windows windows;
        view->getViewManager()->getViewer()->getWindows(windows);
        if (windows.empty()) throw std::runtime_error("SIMDIS failed to create a graphics window");
        for (auto* window : windows) {
            window->setWindowName("HEZARFEN | SIMDIS Mission Visualization");

        }
        std::cout << "[HEZARFEN] Real SIMDIS graphics window realized; " << (replay.empty() ? "LIVE TCP" : "REPLAY (recorded synthetic data)") << std::endl;
        hezarfen::JsonParser parser(scene);
        auto status = [&](const std::string& s, bool disconnect) {
            std::cout << "[HEZARFEN] " << s << std::endl; scene.pushUpdate({{},s,disconnect});
        };
        hezarfen::TcpClient tcp(host,port,[&](const std::string& j) { parser(j); },status);
        std::atomic<bool> replayStop{false}, replayPaused{false};
        if(replay.empty() && host=="127.0.0.1") scene.enableControls(controlPort);
        else if(!replay.empty()) scene.setReplayPause(&replayPaused);
        std::thread replayThread;
        if (replay.empty()) tcp.start();
        else replayThread = std::thread([&]() { hezarfen::replayFile(replay,speed,replayStop,[&](const std::string& j) { parser(j); },status,&replayPaused); });
        int result = 0;
        try { result = viewer->run(); }
        catch (...) {
            scene.stop(); replayStop = true; tcp.stop();
            if (replayThread.joinable()) replayThread.join(); throw;
        }
        scene.stop(); replayStop = true; tcp.stop();
        if (replayThread.joinable()) replayThread.join();
        scene.onFrameUpdate();
        if (!report.empty()) scene.writeReport(report);
        if (windows.front()->makeCurrent()) {
            gui->shutdown();
            windows.front()->releaseContext();
        }
        return result;
    } catch (const std::exception& e) {
        std::cerr << "[HEZARFEN] ERROR: " << e.what() << std::endl; return 1;
    }
}
