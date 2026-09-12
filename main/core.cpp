#include "core.h"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace paper {
bool mp3_header(const uint8_t* p, Mp3Header& h) {
    if(p[0]!=255 || (p[1]&0xe0)!=0xe0) return false;
    int v=(p[1]>>3)&3, layer=(p[1]>>1)&3, b=p[2]>>4, r=(p[2]>>2)&3;
    if(v==1 || layer!=1 || b==0 || b==15 || r==3) return false;
    static const int br1[]={0,32,40,48,56,64,80,96,112,128,160,192,224,256,320};
    static const int br2[]={0,8,16,24,32,40,48,56,64,80,96,112,128,144,160};
    static const int rates[]={44100,48000,32000};
    h.rate=rates[r]/(v==3?1:v==2?2:4);
    h.samples=v==3?1152:576;
    h.bytes=(v==3?144000:72000)*(v==3?br1[b]:br2[b])/h.rate+((p[2]>>1)&1);
    h.channels=(p[3]>>6)==3?1:2;
    return h.bytes>=24 && h.bytes<=1441;
}
uint64_t id3_size(const uint8_t* p) {
    if(std::memcmp(p,"ID3",3) || p[3]<2 || p[3]>4) return 0;
    if((p[6]|p[7]|p[8]|p[9])&128) return 0;
    return 10+((uint64_t)p[6]<<21)+((uint64_t)p[7]<<14)+((uint64_t)p[8]<<7)+p[9]+((p[3]==4 && (p[5]&16))?10:0);
}
void SeekIndex::add(double s,uint64_t b) {
    if(points.empty() || s>=points.back().seconds+1.0) points.push_back({s,b});
}
SeekPoint SeekIndex::before(double s) const {
    // One second of preroll rebuilds the MP3 bit reservoir before audible output.
    s=std::max(0.0,s-1.0);
    auto it=std::upper_bound(points.begin(),points.end(),s,[](double v,const SeekPoint& p){return v<p.seconds;});
    return it==points.begin()?SeekPoint{0,0}:*--it;
}
size_t track_at(const std::vector<Track>& ts,double s) {
    if(ts.empty()) return 0;
    for(size_t i=1;i<ts.size();i++) if(s<ts[i].start) return i-1;
    return ts.size()-1;
}
double resume_position(const Checkpoint& l,double remote,int64_t updated,double duration) {
    double p=l.pending && l.updated_ms>updated?l.position:remote;
    return std::clamp(std::isfinite(p)?p:0.0,0.0,std::max(0.0,duration));
}
ButtonEvent Button::update(bool down,uint64_t now) {
    if(down!=raw){raw=down;changed=now;}
    if(now-changed<35) return ButtonEvent::None;
    // Ignore the initial power-on press until both edges have settled.
    if(!armed){ if(!down) armed=true; return ButtonEvent::None; }
    if(stable!=raw){
        stable=raw;
        if(stable){pressed=now;held=false;}
        else if(!held) return ButtonEvent::Tap;
    }
    if(stable && !held && now-pressed>=hold_ms){held=true;return ButtonEvent::Hold;}
    return ButtonEvent::None;
}
UiResult Controls::input(bool power,ButtonEvent e,int chapters) {
    if(e==ButtonEvent::None) return {};
    if(e==ButtonEvent::Hold){
        if(power) return {Action::Shutdown,0};
        view=View::Playing;cursor=0;return {};
    }
    if(view==View::Playing){
        if(power){view=View::Menu;cursor=0;return {};}
        return {Action::Toggle,0};
    }
    if(view==View::Volume){volume=std::clamp(volume+(power?5:-5),0,100);return {Action::Volume,volume};}
    int count=view==View::Menu?6:view==View::Sleep?4:std::max(1,chapters);
    if(power){cursor=(cursor+1)%count;return {};}
    if(view==View::Sleep){int mins[]={0,15,30,60};view=View::Playing;return {Action::Sleep,mins[cursor]};}
    if(view==View::Chapters){view=View::Playing;return chapters?UiResult{Action::Chapter,cursor}:UiResult{};}
    switch(cursor){
        case 0:view=View::Playing;return {Action::Back30,0};
        case 1:view=View::Playing;return {Action::Forward30,0};
        case 2:view=View::Volume;break;
        case 3:view=View::Chapters;cursor=0;break;
        case 4:view=View::Sleep;cursor=0;break;
        default:view=View::Playing;break;
    }
    return {};
}
std::string url_encode(const std::string& s){
    std::string o;char h[4];
    for(unsigned char c:s){if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~')o+=c;else{snprintf(h,sizeof(h),"%%%02X",c);o+=h;}}
    return o;
}
std::string url_origin(const std::string& s){auto n=s.find("://");if(n==std::string::npos)return {};auto e=s.find('/',n+3);return s.substr(0,e);}
bool valid_base_url(const std::string& s){return (s.rfind("http://",0)==0||s.rfind("https://",0)==0)&&url_origin(s).size()>8&&s.find_first_of("\r\n\t @?#")==std::string::npos;}
std::string resolve_url(const std::string& base,const std::string& ref){
    if(ref.rfind("http://",0)==0||ref.rfind("https://",0)==0)return ref;
    if(ref.rfind("//",0)==0)return {}; // Never forward credentials to a protocol-relative URL.
    if(!ref.empty()&&ref[0]=='/')return url_origin(base)+ref;
    return base+"/"+ref;
}
}
