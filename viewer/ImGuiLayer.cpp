#include "ImGuiLayer.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include <osg/RenderInfo>
#include <osg/State>
#include <osg/View>
#include <osg/GL>
#include <osg/GraphicsContext>
#include <algorithm>
#include <stdexcept>
namespace hezarfen {
namespace {
class DrawPanel : public osg::Camera::DrawCallback {
public:
    explicit DrawPanel(ImGuiLayer* layer) : layer_(layer) {}
    void operator()(osg::RenderInfo& info) const override { layer_->render(info); }
private: osg::ref_ptr<ImGuiLayer> layer_;
};
ImGuiKey key(int k) {
    using E = osgGA::GUIEventAdapter;
    if(k>='a' && k<='z') return ImGuiKey(ImGuiKey_A+k-'a');
    if(k>='A' && k<='Z') return ImGuiKey(ImGuiKey_A+k-'A');
    if(k>='0' && k<='9') return ImGuiKey(ImGuiKey_0+k-'0');
    if(k>=1 && k<=26) return ImGuiKey(ImGuiKey_A+k-1);
    switch(k) {
    case E::KEY_Tab:return ImGuiKey_Tab; case E::KEY_Return:return ImGuiKey_Enter;
    case E::KEY_BackSpace:return ImGuiKey_Backspace; case E::KEY_Delete:return ImGuiKey_Delete;
    case E::KEY_Escape:return ImGuiKey_Escape; case E::KEY_Space:return ImGuiKey_Space;
    case E::KEY_Left:return ImGuiKey_LeftArrow; case E::KEY_Right:return ImGuiKey_RightArrow;
    case E::KEY_Up:return ImGuiKey_UpArrow; case E::KEY_Down:return ImGuiKey_DownArrow;
    case E::KEY_Home:return ImGuiKey_Home; case E::KEY_End:return ImGuiKey_End;
    case E::KEY_Page_Up:return ImGuiKey_PageUp; case E::KEY_Page_Down:return ImGuiKey_PageDown;
    default:return ImGuiKey_None;
    }
}
}
ImGuiLayer::ImGuiLayer(std::function<void()> draw) : draw_(std::move(draw)) {
    IMGUI_CHECKVERSION(); context_=ImGui::CreateContext();
    auto& io=ImGui::GetIO(); io.IniFilename=nullptr; io.LogFilename=nullptr;
    io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard;
    ImFontConfig font; font.SizePixels=15; io.Fonts->AddFontDefault(&font);
    ImGui::StyleColorsDark();
    auto& s=ImGui::GetStyle();
    s.WindowRounding=9; s.ChildRounding=6; s.FrameRounding=5; s.ScrollbarRounding=5;
    s.WindowPadding={14,12}; s.FramePadding={8,5}; s.ItemSpacing={8,8}; s.CellPadding={5,6};
    s.WindowBorderSize=1; s.ScrollbarSize=10;
    s.Colors[ImGuiCol_WindowBg]={.035f,.055f,.085f,.97f};
    s.Colors[ImGuiCol_ChildBg]={.045f,.07f,.105f,1};
    s.Colors[ImGuiCol_Border]={.12f,.21f,.28f,1};
    s.Colors[ImGuiCol_Header]={.07f,.24f,.32f,1};
    s.Colors[ImGuiCol_HeaderHovered]={.1f,.34f,.43f,1};
    s.Colors[ImGuiCol_Button]={.08f,.24f,.32f,1};
    s.Colors[ImGuiCol_ButtonHovered]={.1f,.38f,.47f,1};
    s.Colors[ImGuiCol_FrameBg]={.07f,.12f,.17f,1};
    s.Colors[ImGuiCol_CheckMark]={.28f,.82f,.87f,1};
}
ImGuiLayer::~ImGuiLayer() { if(context_) ImGui::DestroyContext(context_); }
void ImGuiLayer::install(osg::Camera* camera) { camera->addFinalDrawCallback(new DrawPanel(this)); }
void ImGuiLayer::shutdown() {
    if(!context_) return;
    ImGui::SetCurrentContext(context_);
    if(initialized_) { ImGui_ImplOpenGL3_Shutdown(); initialized_=false; }
    ImGui::DestroyContext(context_); context_=nullptr;
}
void ImGuiLayer::render(osg::RenderInfo& info) {
    if(!context_) return;
    ImGui::SetCurrentContext(context_);
    if(!initialized_) {
        if(!ImGui_ImplOpenGL3_Init("#version 330")) throw std::runtime_error("ImGui OpenGL3 initialization failed");
        initialized_=true;
    }
    auto& io=ImGui::GetIO();
    const auto* traits=info.getView()->getCamera()->getGraphicsContext()->getTraits();
    io.DisplaySize={float(traits->width),float(traits->height)};
    const double now=info.getView()->getFrameStamp()->getSimulationTime();
    io.DeltaTime=previousTime_ ? float(std::max(.001,now-previousTime_)) : 1.f/60;
    previousTime_=now;
    // ImGui 1.92 updates its dynamic font textures before its renderer state backup.
    // Preserve the active texture binding too, otherwise OSG's cached binding can
    // cause later SDK glyph uploads to overwrite the ImGui atlas (missing letters).
    info.getState()->unbindPixelBufferObject();
    GLint oldTexture; glGetIntegerv(GL_TEXTURE_BINDING_2D,&oldTexture);
    const GLenum stores[]={GL_UNPACK_ALIGNMENT,GL_UNPACK_ROW_LENGTH,GL_UNPACK_SKIP_ROWS,GL_UNPACK_SKIP_PIXELS};
    GLint oldStores[4];
    for(int i=0;i<4;++i) { glGetIntegerv(stores[i],&oldStores[i]); glPixelStorei(stores[i],i==0 ? 1 : 0); }
    ImGui_ImplOpenGL3_NewFrame(); ImGui::NewFrame(); draw_(); ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glBindTexture(GL_TEXTURE_2D,GLuint(oldTexture));
    for(int i=0;i<4;++i) glPixelStorei(stores[i],oldStores[i]);
    info.getState()->dirtyAllVertexArrays(); info.getState()->dirtyAllAttributes();
}
bool ImGuiLayer::handle(const osgGA::GUIEventAdapter& e, osgGA::GUIActionAdapter&) {
    if(!context_) return false;
    ImGui::SetCurrentContext(context_); auto& io=ImGui::GetIO();
    using E=osgGA::GUIEventAdapter;
    const float y=e.getMouseYOrientation()==E::Y_INCREASING_UPWARDS ? io.DisplaySize.y-e.getY() : e.getY();
    switch(e.getEventType()) {
    case E::MOVE: case E::DRAG: io.AddMousePosEvent(e.getX(),y); return io.WantCaptureMouse;
    case E::PUSH: case E::RELEASE: {
        io.AddMousePosEvent(e.getX(),y);
        const int button=e.getButton()==E::LEFT_MOUSE_BUTTON ? 0 : e.getButton()==E::RIGHT_MOUSE_BUTTON ? 1 : 2;
        io.AddMouseButtonEvent(button,e.getEventType()==E::PUSH); return io.WantCaptureMouse;
    }
    case E::SCROLL:
        io.AddMousePosEvent(e.getX(),y);
        io.AddMouseWheelEvent(0,e.getScrollingMotion()==E::SCROLL_UP ? 1.f : -1.f);
        return io.WantCaptureMouse;
    case E::KEYDOWN: case E::KEYUP: {
        io.AddKeyEvent(ImGuiMod_Ctrl,(e.getModKeyMask()&E::MODKEY_CTRL)!=0);
        io.AddKeyEvent(ImGuiMod_Shift,(e.getModKeyMask()&E::MODKEY_SHIFT)!=0);
        io.AddKeyEvent(ImGuiMod_Alt,(e.getModKeyMask()&E::MODKEY_ALT)!=0);
        const auto k=key(e.getKey()); if(k!=ImGuiKey_None) io.AddKeyEvent(k,e.getEventType()==E::KEYDOWN);
        if(e.getEventType()==E::KEYDOWN && e.getKey()>=32 && e.getKey()<127 && !(e.getModKeyMask()&E::MODKEY_CTRL))
            io.AddInputCharacter(unsigned(e.getKey()));
        return io.WantCaptureKeyboard;
    }
    default:return false;
    }
}
}
