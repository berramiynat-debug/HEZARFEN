#include "SimdisScene.h"
#include "imgui.h"
#include <algorithm>
#include <cctype>
#include <string>
namespace hezarfen {
namespace {
ImVec4 color(SecurityState state) {
    const auto c=securityStateColor(state);
    return {float((c>>24)&255)/255,float((c>>16)&255)/255,float((c>>8)&255)/255,1};
}
const ImVec4 cyan{.30f,.84f,.90f,1}, muted{.58f,.67f,.75f,1};
const ImVec4 red{1,.30f,.34f,1}, amber{1,.74f,.29f,1};
std::string lower(std::string value) {
    std::transform(value.begin(),value.end(),value.begin(),[](unsigned char c){ return char(std::tolower(c)); });
    return value;
}
constexpr auto fixed=ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoResize|
    ImGuiWindowFlags_NoCollapse|ImGuiWindowFlags_NoSavedSettings;
}
void SimdisScene::drawPanel() {
    ++uiFrames_;
    const auto size=ImGui::GetIO().DisplaySize;
    const bool paused=replayPaused_ ? replayPaused_->load() : controlStatus_.paused;
    const bool localReplay=replayPaused_!=nullptr;
    const bool replayActive=localReplay && sourceStatus_.find("REPLAY COMPLETE")==std::string::npos && sourceStatus_.find("ERROR")==std::string::npos;
    const bool controlsAvailable=localReplay || (control_ && controlStatus_.supported && !state_.stale);
    const bool busy=control_ && control_->busy();
    const float panelWidth=std::min(364.f,size.x*.43f);
    ImGui::SetNextWindowPos({12,12}); ImGui::SetNextWindowSize({size.x-24,76});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{14,7});
    ImGui::Begin("HEZARFEN header",nullptr,fixed);
    ImGui::SetWindowFontScale(1.25f); ImGui::TextColored(cyan,"HEZARFEN"); ImGui::SetWindowFontScale(1);
    ImGui::SameLine(); ImGui::TextColored(muted," / SIMDIS Mission Visualization");
    ImGui::TextColored(muted,"SYNTHETIC DEMONSTRATOR  |  Backend decisions only");
    ImGui::SetCursorPos({std::max(430.f,size.x-605),15});
    ImGui::TextColored(mode_=="REPLAY" ? amber : cyan,"%s",mode_.c_str());
    ImGui::SameLine(); ImGui::Text("t = %.1f s  /  #%llu",state_.time,static_cast<unsigned long long>(state_.seq));
    ImGui::SetCursorPos({size.x-326,15});
    if(ImGui::Button("Mission")) cameraAction_=CameraAction::Mission;
    ImGui::SameLine(); if(ImGui::Button("Focus")) cameraAction_=CameraAction::Selected;
    ImGui::SameLine(); if(ImGui::Button("3D")) cameraAction_=CameraAction::Perspective;
    ImGui::SameLine(); if(ImGui::Button("2D")) cameraAction_=CameraAction::Overhead;
    ImGui::SetCursorPos({std::max(430.f,size.x-605),42});
    ImGui::BeginDisabled(!controlsAvailable || busy || paused || (localReplay ? !replayActive : !controlStatus_.running));
    if(ImGui::Button("Duraklat")) { if(localReplay) replayPaused_->store(true); else control_->request("pause"); }
    ImGui::EndDisabled(); ImGui::SameLine();
    ImGui::BeginDisabled(!controlsAvailable || busy || !paused);
    if(ImGui::Button("Devam et")) { if(localReplay) replayPaused_->store(false); else control_->request("resume"); }
    ImGui::EndDisabled(); ImGui::SameLine();
    if(paused) ImGui::TextColored(amber,"PAUSED");
    else if(busy) ImGui::TextDisabled("Applying...");
    else if(!controlsAvailable) ImGui::TextDisabled("Controls unavailable");
    ImGui::End(); ImGui::PopStyleVar();

    ImGui::SetNextWindowPos({12,98}); ImGui::SetNextWindowSize({panelWidth,std::max(240.f,size.y-110)});
    ImGui::Begin("Mission monitor",nullptr,fixed);
    ImGui::TextColored(state_.stale ? red : cyan,state_.stale ? "STALE DATA" : "DATA CURRENT");
    ImGui::SameLine(); ImGui::TextColored(muted," / %s",paused ? "PAUSED" : state_.running ? (*state_.running ? "RUNNING" : "IDLE") : "SYNCHRONIZING");
    if(control_ && !controlStatus_.error.empty()) ImGui::TextWrapped("Controls: %s",controlStatus_.error.c_str());
    if(state_.stale) ImGui::TextWrapped("%s",state_.status.c_str());
    if(mode_=="REPLAY" && !sourceStatus_.empty()) ImGui::TextWrapped("%s",sourceStatus_.c_str());
    if(!state_.runId.empty()) ImGui::TextWrapped("Run: %s",state_.runId.c_str());
    ImGui::Spacing(); ImGui::TextColored(muted,"PLATFORMS  /  %zu",state_.platforms.size());
    if(ImGui::BeginTable("Platforms",4,ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerH|ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Platform",0,1.1f); ImGui::TableSetupColumn("Security",0,1.3f);
        ImGui::TableSetupColumn("Link",0,.7f); ImGui::TableSetupColumn("Alt",0,.7f); ImGui::TableHeadersRow();
        for(const auto& [id,p] : state_.platforms) {
            ImGui::PushID(id.c_str()); ImGui::TableNextRow(); ImGui::TableNextColumn();
            if(ImGui::Selectable(id.c_str(),selected_==id,ImGuiSelectableFlags_SpanAllColumns)) selected_=id;
            if(ImGui::IsItemHovered()) {
                ImGui::BeginTooltip(); ImGui::Text("%s / %s",p.value.kind.c_str(),p.value.role.c_str());
                ImGui::Text("WGS84: %.6f, %.6f",p.value.lat_deg,p.value.lon_deg);
                ImGui::Text("Altitude %.1f m | Heading %.1f deg",p.value.alt_m,p.value.heading_deg);
                if(!p.lastDecision.empty()) ImGui::TextWrapped("%s",p.lastDecision.c_str());
                ImGui::TextUnformatted("Select row, then Focus to inspect in 3D."); ImGui::EndTooltip();
            }
            ImGui::TableNextColumn(); ImGui::TextColored(color(p.value.sec),"%s",securityStateLabel(p.value.sec).c_str());
            ImGui::TableNextColumn(); ImGui::TextColored(p.link=="DOWN" ? amber : muted,"%s",p.link.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.0fm",p.value.alt_m); ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::Spacing(); ImGui::TextColored(muted,"SURFACE CONTACTS  /  %zu",state_.contacts.size());
    if(state_.contacts.empty()) ImGui::TextDisabled("No contacts reported");
    for(const auto& [id,c] : state_.contacts) {
        ImGui::Text("%s",id.c_str()); ImGui::SameLine(); ImGui::TextColored(amber,"%s",c.status.c_str());
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Source: %s | WGS84 %.5f, %.5f",c.source.c_str(),c.lat_deg,c.lon_deg);
    }
    if(ImGui::CollapsingHeader("Security legend")) {
        for(auto state : {SecurityState::Trusted,SecurityState::Watch,SecurityState::Restricted,SecurityState::Quarantined,SecurityState::Revoked}) {
            if(state!=SecurityState::Trusted && state!=SecurityState::Quarantined) ImGui::SameLine();
            ImGui::TextColored(color(state),"%s",securityStateLabel(state).c_str());
        }
    }
    ImGui::Spacing(); ImGui::Separator(); ImGui::TextColored(cyan,"BACKEND EVENTS");
    ImGui::SetNextItemWidth(std::max(100.f,ImGui::GetContentRegionAvail().x-82));
    ImGui::InputTextWithHint("##eventFilter","Filter events / evidence",eventFilter_,sizeof(eventFilter_));
    ImGui::SameLine(); ImGui::Checkbox("Follow",&followEvents_);
    if(ImGui::BeginChild("Ordered events",{0,0},ImGuiChildFlags_Borders)) {
        const auto filter=lower(eventFilter_); bool any=false;
        for(const auto& event : state_.events) {
            if(!filter.empty() && lower(event).find(filter)==std::string::npos) continue;
            any=true;
            const bool rejected=event.find("REJECTED")!=std::string::npos;
            ImGui::PushStyleColor(ImGuiCol_Text,rejected ? amber : ImVec4(.83f,.89f,.94f,1));
            ImGui::TextWrapped("%s",event.c_str()); ImGui::PopStyleColor(); ImGui::Separator();
        }
        if(!any) ImGui::TextWrapped(state_.events.empty() ? "Waiting for backend events." : "No events match your filter.");
        const auto tail=state_.events.empty() ? std::string{} : state_.events.back();
        if(followEvents_ && shownEvent_!=tail) ImGui::SetScrollHereY(1);
    }
    ImGui::EndChild(); shownEvent_=state_.events.empty() ? std::string{} : state_.events.back(); ImGui::End();

    ImGui::SetNextWindowPos({panelWidth+32,size.y-36});
    ImGui::SetNextWindowSize({std::max(100.f,size.x-panelWidth-44),26});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{8,5});
    ImGui::Begin("Camera help",nullptr,fixed|ImGuiWindowFlags_NoInputs|ImGuiWindowFlags_NoBackground);
    ImGui::TextColored(muted,"Drag: rotate / pan   Wheel: zoom   Home: mission   1 / 2: 3D / 2D");
    ImGui::End(); ImGui::PopStyleVar();
}
}
