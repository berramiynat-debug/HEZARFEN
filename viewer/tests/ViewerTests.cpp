#include "Protocol.h"
#include "Replay.h"
#include "TcpClient.h"
#include "Coordinates.h"
#include "third_party/json.hpp"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
using namespace hezarfen;
using namespace std::chrono_literals;
namespace {
int checks = 0;
void check(bool value, const char* why) { ++checks; if (!value) throw std::runtime_error(why); }
template<class Fn> void rejects(Fn f) { bool threw=false; try { f(); } catch (const std::exception&) { threw=true; } check(threw,"invalid input accepted"); }
bool sendAll(SOCKET s, const char* bytes, int count) {
    int offset=0; while (offset<count) { int n=send(s,bytes+offset,count-offset,0); if(n<=0) return false; offset+=n; } return true;
}
void transport(const std::string& snapshot) {
    WSADATA data{}; check(WSAStartup(MAKEWORD(2,2),&data)==0,"WSA startup");
    SOCKET listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    sockaddr_in address{}; address.sin_family=AF_INET; address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    check(bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0,"test bind");
    int size=sizeof(address); getsockname(listener,reinterpret_cast<sockaddr*>(&address),&size);
    check(listen(listener,2)==0,"listen");
    std::atomic<int> received{0}, disconnected{0}; std::atomic<bool> serverOk{true};
    std::thread server([&]() {
        for (int session=0;session<2;++session) {
            fd_set set; FD_ZERO(&set); FD_SET(listener,&set); timeval timeout{8,0};
            if(select(0,&set,nullptr,nullptr,&timeout)<=0) {serverOk=false; break;}
            SOCKET peer=accept(listener,nullptr,nullptr);
            uint32_t header=htonl(static_cast<uint32_t>(snapshot.size()));
            const auto* h=reinterpret_cast<const char*>(&header);
            // Exercise exact reads with each header byte and small UTF-8 body fragments.
            for(int i=0;i<4;++i) {if(!sendAll(peer,h+i,1))serverOk=false; std::this_thread::sleep_for(2ms);}
            for(std::size_t i=0;i<snapshot.size();i+=7)
                if(!sendAll(peer,snapshot.data()+i,static_cast<int>(std::min<std::size_t>(7,snapshot.size()-i))))serverOk=false;
            if(session==0) {header=htonl(MAX_PAYLOAD+1);sendAll(peer,reinterpret_cast<const char*>(&header),4);}
            else std::this_thread::sleep_for(250ms);
            closesocket(peer);
        }
        closesocket(listener);
    });
    TcpClient client("127.0.0.1",ntohs(address.sin_port),[&](const std::string& s) {
        auto message=parseMessage(s); if(message.type=="SNAPSHOT") ++received;
    },[&](const std::string&,bool stale) {if(stale)++disconnected;});
    client.start(); const auto deadline=std::chrono::steady_clock::now()+8s;
    while(received<2 && std::chrono::steady_clock::now()<deadline) std::this_thread::sleep_for(10ms);
    const auto stopAt=std::chrono::steady_clock::now(); client.stop();
    const auto stopTime=std::chrono::steady_clock::now()-stopAt;
    server.join(); WSACleanup();
    check(serverOk,"fragment server failed"); check(received==2,"fragmented read / oversize reconnect failed");
    check(disconnected>=2,"connection states missing"); check(stopTime<1s,"socket stop blocked");
}
}
int main(int argc,char** argv) {
    try {
        check(argc==2,"recording path required");
        std::ifstream in(argv[1]); std::string first; std::getline(in,first);
        const auto snapshot=parseMessage(first);
        check(snapshot.platforms.size()==6,"snapshot must contain six platforms");
        check(validateReplay(argv[1])>900,"complete recording validation");
        MissionState state;
        auto hb=parseMessage(R"({"v":1,"type":"HEARTBEAT","seq":2,"t_sec":0,"data":{"running":false}})");
        check(!state.apply(hb),"heartbeat before snapshot"); check(state.apply(snapshot),"snapshot rejected");
        check(!state.apply(snapshot),"duplicate seq applied"); check(state.apply(hb),"heartbeat rejected");
        check(state.running && !*state.running,"false heartbeat lost");
        auto malformed=nlohmann::json::parse(first);
        for(const auto& key : {"id","kind","role","lat_deg","lon_deg","alt_m","heading_deg","security_state"}) {
            auto bad=malformed; bad["data"]["platforms"][0].erase(key); rejects([&]{parseMessage(bad.dump());});
        }
        for(auto bad : {std::string("{"),std::string("{\"v\":1,\"v\":1}"),std::string("[]"),std::string(MAX_PAYLOAD+1,'x')})
            rejects([&]{parseMessage(bad);});
        for(auto value : {"Trustedx","trusted","Unknown"}) rejects([&]{parseSecurityState(value);});
        auto bad=malformed; bad["v"]=2; rejects([&]{parseMessage(bad.dump());});
        bad=malformed; bad["seq"]=-1; rejects([&]{parseMessage(bad.dump());});
        bad=malformed; bad["data"]["platforms"][0]["lat_deg"]=91; rejects([&]{parseMessage(bad.dump());});
        bad=malformed; bad["data"]["platforms"][0]["lat_deg"]="40"; rejects([&]{parseMessage(bad.dump());});
        bad=malformed; bad["data"]["platforms"][0]["alt_m"]=10; rejects([&]{parseMessage(bad.dump());});
        bad=malformed; bad["data"]["platforms"][3]["alt_m"]=0; rejects([&]{parseMessage(bad.dump());});
        std::string invalid=first; invalid.insert(invalid.find("COMMAND_AUTHORITY"),1,char(0xFF)); rejects([&]{parseMessage(invalid);});
        auto unicode=malformed; unicode["data"]["platforms"][0]["role"]="Komuta \"İzleme\"";
        check(parseMessage(unicode.dump()).platforms[0].role=="Komuta \"İzleme\"","UTF-8 / escapes");
        state.disconnected("test"); check(state.stale && !state.apply(hb),"stale until snapshot");
        check(state.apply(snapshot),"new connection sequence reset");
        // Replay the exact backend recording and assert trust and connectivity stay independent.
        in.clear();in.seekg(0); state=MissionState{}; std::string line; uint64_t previous=0; int count=0;
        bool down=false,restored=false,restricted=false,watch=false;
        while(std::getline(in,line)) {
            auto m=parseMessage(line); check(m.seq>previous,"sequence order");previous=m.seq;
            check(state.apply(m),"backend message rejected"); ++count;
            if(m.security && m.security->subject=="UAV-02") {
                watch |= state.platforms.at("UAV-02").value.sec==SecurityState::Watch;
                restricted |= state.platforms.at("UAV-02").value.sec==SecurityState::Restricted;
            }
            if(m.link) {
                check(state.platforms.at("UAV-03").value.sec==SecurityState::Trusted,"link loss changed trust");
                if(m.link->status=="DOWN") down=true; else restored=true;
            }
        }
        check(watch && restricted && down && restored,"full demo transitions");
        check(state.platforms.size()==6 && state.contacts.size()==1,"final scene cardinality");
        check(state.contacts.at("CONTACT-01").status=="Unverified","contact promoted without evidence");
        check(state.platforms.at("UAV-02").value.sec==SecurityState::Restricted,"restriction lost");
        check(state.platforms.at("UAV-02").lastDecision.find("REJECTED")!=std::string::npos,"safety telemetry hid command rejection");
        check(state.running && !*state.running,"end of recording running flag");
        state.disconnected("reconnect"); check(state.apply(snapshot),"snapshot restore");
        check(state.contacts.empty() && state.platforms.at("UAV-03").link=="UP","snapshot not authoritative");
        for(const auto& p : snapshot.platforms) {
            auto ecef=toEcef(p.lat_deg,p.lon_deg,p.alt_m,p.heading_deg);simCore::Coordinate roundtrip;
            check(simCore::CoordinateConverter::convertEcefToGeodetic(ecef,roundtrip)==0,"ECEF inverse");
            check(std::abs(roundtrip.lat()*simCore::RAD2DEG-p.lat_deg)<1e-8,"latitude roundtrip");
            check(std::abs(roundtrip.lon()*simCore::RAD2DEG-p.lon_deg)<1e-8,"longitude roundtrip");
            check(std::abs(roundtrip.alt()-p.alt_m)<1e-5,"altitude roundtrip");
            check(std::abs(roundtrip.yaw()*simCore::RAD2DEG-p.heading_deg)<1e-4,"heading roundtrip");
        }
        auto equator=toEcef(0,0,0,0); check(std::abs(equator.x()-6378137)<1e-5 && std::abs(equator.y())<1e-5,"known WGS84 ECEF point");
        transport(unicode.dump());
        std::cout << "PASS " << checks << " checks; " << count << " actual backend messages; coordinates, state, UTF-8, fragmented TCP, oversized-frame reconnect, shutdown\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<"\n";return 1;}
}
