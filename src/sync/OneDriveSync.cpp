#include "OneDriveSync.h"
#include "../network/HttpClient.h"
#include "../network/JsonHelper.h"
#include "../database/DatabaseManager.h"
#include "../logging/Logger.h"
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace RomCloud {
static const char* OD_B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
static std::string toBase64Url(const std::string& in) {
    std::string out; int val=0,valb=-6;
    for (unsigned char c: in){val=(val<<8)+c;valb+=8;while(valb>=0){out.push_back(OD_B64[(val>>valb)&0x3F]);valb-=6;}}
    if(valb>-6) out.push_back(OD_B64[((val<<8)>>(valb+8))&0x3F]);
    while(out.size()%4) out.push_back('=');
    return out;
}
bool oneDriveIsShareLink(const std::string& url){
    std::string l=url; std::transform(l.begin(),l.end(),l.begin(),::tolower);
    return l.find("1drv.ms")!=std::string::npos||l.find("onedrive.live.com")!=std::string::npos||l.find("sharepoint.com")!=std::string::npos;
}
std::string oneDriveEncodeShareToken(const std::string& s){
    std::string t=s;
    while(!t.empty()&&(t.front()==' '||t.front()=='\t'||t.front()=='\r'||t.front()=='\n'))t.erase(0,1);
    while(!t.empty()&&(t.back()==' '||t.back()=='\t'||t.back()=='\r'||t.back()=='\n'))t.pop_back();
    return "u!"+toBase64Url(t);
}
struct OdItem{std::string id;std::string name;uint64_t size=0;bool isFolder=false;};
static std::vector<OdItem> odList(const std::string& tok,const std::string& itemId){
    std::vector<OdItem> out;
    std::string url=itemId.empty()?"https://api.onedrive.com/v1.0/shares/"+tok+"/driveItem/children?$top=200":"https://api.onedrive.com/v1.0/shares/"+tok+"/driveItem/items/"+itemId+"/children?$top=200";
    HttpResponse r=HttpClient::instance().get(url,{"User-Agent: Mozilla/5.0","Accept: application/json"},30);
    if(!r.success||r.body.empty()){Logger::warn("OneDrive list fail HTTP "+std::to_string(r.statusCode)+" "+r.error);return out;}
    for(auto& o: JsonHelper::extractArrayObjects(r.body,"value")){
        OdItem it; it.id=JsonHelper::extractString(o,"id"); it.name=JsonHelper::extractString(o,"name");
        it.size=JsonHelper::extractUInt64(o,"size",0); it.isFolder=(o.find("\"folder\"")!=std::string::npos);
        if(!it.id.empty()&&!it.name.empty()) out.push_back(it);
    }
    return out;
}
static std::string odExt(const std::string& f){size_t d=f.find_last_of('.');if(d==std::string::npos)return "";std::string e=f.substr(d+1);std::transform(e.begin(),e.end(),e.begin(),::tolower);return e;}
static bool odExtOk(const SystemRecord& s,const std::string& e){
    if(s.extList.empty())return true;
    std::stringstream ss(s.extList);std::string t;
    while(std::getline(ss,t,'|')){std::stringstream s2(t);std::string p;while(std::getline(s2,p,',')){
        while(!p.empty()&&p.front()==' ')p.erase(0,1);while(!p.empty()&&(p.back()==' '||p.back()==','))p.pop_back();
        std::transform(p.begin(),p.end(),p.begin(),::tolower);if(p==e)return true;}}
    return false;
}
static std::string odTitle(const std::string& fn){
    std::string b=fn;size_t d=b.find_last_of('.');if(d!=std::string::npos)b=b.substr(0,d);
    std::string c;bool p=false,q=false;
    for(char ch:b){if(ch=='(')p=true;else if(ch==')'){p=false;continue;}else if(ch=='[')q=true;else if(ch==']'){q=false;continue;}else if(!p&&!q)c+=ch;}
    while(!c.empty()&&(c.back()==' '||c.back()=='-'||c.back()=='_'))c.pop_back();
    while(!c.empty()&&(c.front()==' '||c.front()=='-'||c.front()=='_'))c.erase(0,1);
    return c.empty()?fn:c;
}
static std::string odMatch(const std::string& n){
    std::string s=n;std::transform(s.begin(),s.end(),s.begin(),::tolower);
    if(s.find("gba")!=std::string::npos)return "GBA";
    if(s.find("gbc")!=std::string::npos)return "GBC";
    if(s.find("game boy")!=std::string::npos||s=="gb")return "GB";
    if(s.find("snes")!=std::string::npos||s.find("sfc")!=std::string::npos||s.find("super nintendo")!=std::string::npos)return "SFC";
    if(s.find("genesis")!=std::string::npos||s.find("mega drive")!=std::string::npos||s=="md")return "MD";
    if(s.find("n64")!=std::string::npos||s.find("nintendo 64")!=std::string::npos)return "N64";
    if(s.find("nds")!=std::string::npos||s.find("nintendo ds")!=std::string::npos)return "NDS";
    if(s.find("playstation")!=std::string::npos||s.find("ps1")!=std::string::npos||s.find("psx")!=std::string::npos)return "PS";
    if(s.find("psp")!=std::string::npos)return "PSP";
    if(s.find("dreamcast")!=std::string::npos)return "DC";
    if(s.find("saturn")!=std::string::npos)return "SS";
    if(s.find("game gear")!=std::string::npos||s.find("gamegear")!=std::string::npos||s=="gg")return "GG";
    if(s.find("master system")!=std::string::npos||s=="ms"||s=="sms")return "MS";
    if(s.find("sega cd")!=std::string::npos||s.find("segacd")!=std::string::npos)return "SEGACD";
    if(s.find("neo geo")!=std::string::npos||s.find("neogeo")!=std::string::npos)return "NEOGEO";
    if(s.find("mame")!=std::string::npos||s.find("arcade")!=std::string::npos||s.find("fbneo")!=std::string::npos)return "ARCADE";
    if(s.find("pce")!=std::string::npos||s.find("pc engine")!=std::string::npos||s.find("turbografx")!=std::string::npos)return "PCE";
    if(s.find("atari")!=std::string::npos||s.find("2600")!=std::string::npos)return "ATARI2600";
    if(s.find("nes")!=std::string::npos||s.find("famicom")!=std::string::npos||s=="fc")return "FC";
    return "";
}
int oneDriveSyncShare(const std::string& shareUrl){
    std::string tok=oneDriveEncodeShareToken(shareUrl);
    Logger::info("OneDrive share sync started.");
    auto systems=DatabaseManager::instance().getSystems(false);
    std::unordered_map<std::string,SystemRecord> byCode;
    for(auto& s:systems)byCode[s.code]=s;
    std::vector<OdItem> root=odList(tok,"");
    if(root.empty()){Logger::error("OneDrive share empty/unreachable. Check Anyone-with-link permission.");return -1;}
    DatabaseManager::instance().beginTransaction();
    int indexed=0;
    for(auto& it:root){
        if(it.isFolder||it.name.empty()||it.name[0]=='.')continue;
        std::string e=odExt(it.name); if(e.empty())continue;
        const SystemRecord* m=nullptr; int hits=0;
        for(auto& s:systems) if(odExtOk(s,e)){hits++;m=&s;}
        if(hits==1&&m){GameRecord g;g.systemId=m->id;g.filename=it.name;g.title=odTitle(it.name);g.sizeBytes=it.size;g.cloudFileId="od1|"+tok+"|"+it.id;g.localState=GameState::CLOUD;DatabaseManager::instance().upsertGame(g);indexed++;}
    }
    for(auto& f:root){
        if(!f.isFolder)continue;
        std::string code=odMatch(f.name); if(code.empty())continue;
        auto sit=byCode.find(code); if(sit==byCode.end())continue;
        for(auto& it:odList(tok,f.id)){
            if(it.isFolder||it.name.empty()||it.name[0]=='.')continue;
            if(!odExtOk(sit->second,odExt(it.name)))continue;
            GameRecord g;g.systemId=sit->second.id;g.filename=it.name;g.title=odTitle(it.name);g.sizeBytes=it.size;g.cloudFileId="od1|"+tok+"|"+it.id;g.localState=GameState::CLOUD;DatabaseManager::instance().upsertGame(g);indexed++;
        }
    }
    DatabaseManager::instance().commitTransaction();
    auto now=std::chrono::system_clock::now(); std::time_t tt=std::chrono::system_clock::to_time_t(now);
    std::stringstream ss; ss<<std::put_time(std::localtime(&tt),"%Y-%m-%d %H:%M");
    DatabaseManager::instance().setSetting("last_cloud_sync_time",ss.str());
    Logger::info("OneDrive share sync complete. Indexed: "+std::to_string(indexed));
    return indexed;
}
} // namespace RomCloud
