#include "MissionControl.h"
#include "third_party/json.hpp"
#include <windows.h>
#include <winhttp.h>
#include <chrono>
#include <stdexcept>
namespace hezarfen {
namespace {
struct Handle {
    HINTERNET value{};
    ~Handle() { if(value) WinHttpCloseHandle(value); }
};
std::string httpRequest(HINTERNET connection,const wchar_t* method,const std::wstring& path) {
    Handle req{WinHttpOpenRequest(connection,method,path.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,0)};
    if(!req.value || !WinHttpSendRequest(req.value,WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,0) ||
        !WinHttpReceiveResponse(req.value,nullptr)) throw std::runtime_error("Presentation backend unavailable");
    DWORD code{},length=sizeof(code);
    if(!WinHttpQueryHeaders(req.value,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&code,&length,WINHTTP_NO_HEADER_INDEX))
        throw std::runtime_error("Cannot read control response");
    std::string response; char chunk[4096]; DWORD count{};
    do {
        if(!WinHttpReadData(req.value,chunk,sizeof(chunk),&count)) throw std::runtime_error("Cannot read control data");
        if(response.size()+count>1024*1024) throw std::runtime_error("Control response exceeds 1 MiB");
        response.append(chunk,count);
    }while(count);
    if(code!=200) {
        const auto j=nlohmann::json::parse(response,nullptr,false);
        throw std::runtime_error(j.is_object() ? j.value("error",std::string("Control request rejected")) : "Control request rejected");
    }
    return response;
}
}
MissionControl::MissionControl(uint16_t port,Callback callback) : port_(port),callback_(std::move(callback)) {}
MissionControl::~MissionControl() { stop(); }
void MissionControl::start() { if(!worker_.joinable()) { stop_=false; worker_=std::thread(&MissionControl::run,this); } }
void MissionControl::stop() { stop_=true; wake_.notify_all(); if(worker_.joinable()) worker_.join(); }
void MissionControl::request(const std::string& action) {
    if(action!="pause" && action!="resume" && action!="restart") return;
    if(busy_.exchange(true)) return;
    { std::lock_guard<std::mutex> lock(mutex_); action_=action; }
    wake_.notify_one();
}
void MissionControl::run() {
    Handle session{WinHttpOpen(L"HEZARFEN Mission Control",WINHTTP_ACCESS_TYPE_NO_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0)};
    if(!session.value) { callback_({false,false,false,0,"Cannot initialize local controls"}); return; }
    WinHttpSetTimeouts(session.value,500,500,500,500);
    Handle connection{WinHttpConnect(session.value,L"127.0.0.1",port_,0)};
    if(!connection.value) { callback_({false,false,false,0,"Cannot connect local controls"}); return; }
    ControlStatus current;
    while(!stop_) {
        std::string action;
        { std::lock_guard<std::mutex> lock(mutex_); action.swap(action_); }
        try {
            std::string actionError;
            if(!action.empty()) {
                try { httpRequest(connection.value,L"POST",L"/action/"+std::wstring(action.begin(),action.end())); }
                catch(const std::exception& e) { actionError=e.what(); }
            }
            const auto state=nlohmann::json::parse(httpRequest(connection.value,L"GET",L"/state"));
            current.supported=state.value("pause_supported",false);
            current.restartSupported=state.value("restart_supported",false);
            current.paused=state.value("paused",false);
            current.running=state.at("running").get<bool>(); current.time=state.at("t_sec").get<double>();
            current.error=actionError;
        } catch(const std::exception& e) { current.supported=false; current.error=e.what(); }
        if(!action.empty()) busy_=false;
        if(!stop_) callback_(current);
        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait_for(lock,std::chrono::milliseconds(400),[&]{return stop_ || !action_.empty();});
    }
    busy_=false;
}
}
