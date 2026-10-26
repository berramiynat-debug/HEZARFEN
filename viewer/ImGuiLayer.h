#pragma once
#include <functional>
#include <osg/Camera>
#include <osgGA/GUIEventHandler>
struct ImGuiContext;
namespace hezarfen {
// Single-threaded OSG integration: event input before traversal, ImGui after the scene.
class ImGuiLayer : public osgGA::GUIEventHandler {
public:
    explicit ImGuiLayer(std::function<void()> draw);
    void install(osg::Camera* overlay);
    bool handle(const osgGA::GUIEventAdapter&, osgGA::GUIActionAdapter&) override;
    void render(osg::RenderInfo& info);
    void shutdown(); // call while the graphics context is current
protected:
    ~ImGuiLayer() override;
private:
    ImGuiContext* context_{};
    std::function<void()> draw_;
    double previousTime_{};
    bool initialized_{false}, fontsWarmed_{false};
};
}
