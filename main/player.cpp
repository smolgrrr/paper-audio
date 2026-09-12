#include "app.h"
#include "esp_mp3_dec.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <cstring>
#include <cmath>
#include <deque>

namespace paper {
static QueueHandle_t commands;
bool enqueue(Command c){auto p=new Command(std::move(c));if(!commands||xQueueSend(commands,&p,0)!=pdTRUE){delete p;return false;}return true;}
struct Headers {std::string range;};
static esp_err_t header_event(esp_http_client_event_t* e){if(e->event_id==HTTP_EVENT_ON_HEADER&&e->header_key&&!strcasecmp(e->header_key,"Content-Range"))static_cast<Headers*>(e->user_data)->range=e->header_value;return ESP_OK;}
class Stream {
    std::mutex mutex;std::vector<uint8_t> ring=std::vector<uint8_t>(256*1024);
    size_t head=0,tail=0,used=0;uint64_t generation=0,wanted_offset=0,read_offset=0;
    bool active=false,ended=false;std::string url,failure;Config cfg;
public:
    void start(const Config& c,const std::string& u,uint64_t offset){std::lock_guard<std::mutex> l(mutex);cfg=c;url=u;wanted_offset=read_offset=offset;head=tail=used=0;active=true;ended=false;failure.clear();generation++;}
    void stop(){std::lock_guard<std::mutex> l(mutex);active=false;generation++;used=0;}
    uint64_t offset(){std::lock_guard<std::mutex> l(mutex);return read_offset;}
    std::string error(){std::lock_guard<std::mutex> l(mutex);return failure;}
    // No bytes are consumed on interruption. This keeps partial MP3 frames intact.
    int peek(uint8_t* p,size_t n){
        for(;;){
            {std::lock_guard<std::mutex> l(mutex);if(used>=n){for(size_t i=0;i<n;i++)p[i]=ring[(tail+i)%ring.size()];return 1;}if(!failure.empty())return -1;if(ended)return -2;}
            if(uxQueueMessagesWaiting(commands))return 0;
            status("Buffering");vTaskDelay(pdMS_TO_TICKS(15));
        }
    }
    void consume(size_t n){std::lock_guard<std::mutex> l(mutex);n=std::min(n,used);tail=(tail+n)%ring.size();used-=n;read_offset+=n;}
    void run(){
        uint64_t epoch=UINT64_MAX,position=0;esp_http_client_handle_t h=nullptr;Headers headers;Config c;std::string u;uint8_t data[8192];
        for(;;){
            bool enabled;size_t available;uint64_t seq;
            {std::lock_guard<std::mutex> l(mutex);seq=generation;enabled=active;available=ring.size()-used;if(seq!=epoch){c=cfg;u=url;position=wanted_offset;}}
            if(seq!=epoch){if(h)esp_http_client_cleanup(h);h=nullptr;epoch=seq;}
            if(!enabled){vTaskDelay(pdMS_TO_TICKS(30));continue;}
            {std::lock_guard<std::mutex> l(mutex);if(ended||!failure.empty()){enabled=false;}}
            if(!enabled||!available){vTaskDelay(pdMS_TO_TICKS(15));continue;}
            if(!h){
                if(!wifi_connected()){vTaskDelay(pdMS_TO_TICKS(300));continue;}
                headers.range.clear();esp_http_client_config_t hc={};hc.url=u.c_str();hc.timeout_ms=4000;hc.crt_bundle_attach=esp_crt_bundle_attach;hc.disable_auto_redirect=true;hc.buffer_size=8192;hc.event_handler=header_event;hc.user_data=&headers;
                h=esp_http_client_init(&hc);if(!h){vTaskDelay(pdMS_TO_TICKS(1000));continue;}
                std::string auth="Bearer "+c.token,range="bytes="+std::to_string(position)+"-";
                esp_http_client_set_header(h,"Authorization",auth.c_str());esp_http_client_set_header(h,"Range",range.c_str());esp_http_client_set_header(h,"Accept-Encoding","identity");
                if(esp_http_client_open(h,0)!=ESP_OK||esp_http_client_fetch_headers(h)<0){esp_http_client_cleanup(h);h=nullptr;vTaskDelay(pdMS_TO_TICKS(1000));continue;}
                int code=esp_http_client_get_status_code(h);unsigned long long begin=0,end=0,total=0;
                bool range_ok=code==206&&sscanf(headers.range.c_str(),"bytes %llu-%llu/%llu",&begin,&end,&total)==3&&begin==position&&end>=begin;
                if(!range_ok && !(code==200&&position==0)){
                    std::lock_guard<std::mutex> l(mutex);if(epoch==generation)failure=code==401||code==403?"Sign in again on phone":code==200?"Server does not support seeking":"Audio HTTP "+std::to_string(code);
                    esp_http_client_cleanup(h);h=nullptr;continue;
                }
            }
            int n=esp_http_client_read(h,(char*)data,std::min(available,sizeof(data)));
            if(n>0){std::lock_guard<std::mutex> l(mutex);if(epoch==generation){for(int i=0;i<n;i++){ring[head]=data[i];head=(head+1)%ring.size();}used+=n;position+=n;}}
            else {bool done=n==0&&esp_http_client_is_complete_data_received(h);esp_http_client_cleanup(h);h=nullptr;if(done){std::lock_guard<std::mutex> l(mutex);if(epoch==generation)ended=true;}else vTaskDelay(pdMS_TO_TICKS(1000));}
        }
    }
};
static Stream* stream;
struct FrameReader {
    uint64_t skip=0;size_t junk=0;
    int next(uint8_t* frame,Mp3Header& header){
        uint8_t p[10];
        for(;;){
            int r=stream->peek(p,10);if(r!=1)return r;
            if(skip){size_t n=std::min<uint64_t>(skip,10);stream->consume(n);skip-=n;continue;}
            uint64_t tag=id3_size(p);if(tag){if(tag>32*1024*1024)return -3;skip=tag;continue;}
            if(!mp3_header(p,header)){stream->consume(1);if(++junk>1024*1024)return -3;continue;}
            r=stream->peek(frame,header.bytes);if(r!=1)return r;stream->consume(header.bytes);junk=0;return 1;
        }
    }
};
struct SyncJob {Checkpoint cp;std::string session;double listened=0;bool close=false;};
static std::mutex sync_mutex;
static std::deque<SyncJob> jobs;
static std::atomic<int> sync_active{0};
static void sync_job(const SyncJob& j){save_checkpoint(j.cp);std::lock_guard<std::mutex> l(sync_mutex);jobs.push_back(j);}
static void sync_task(void*){
    for(;;){
        SyncJob j;bool have=false;{std::lock_guard<std::mutex> l(sync_mutex);if(!jobs.empty()){j=jobs.front();jobs.pop_front();have=true;sync_active++;}}
        if(!have){vTaskDelay(pdMS_TO_TICKS(100));continue;}
        Json data(cJSON_CreateObject());cJSON_AddNumberToObject(data.get(),"currentTime",j.cp.position);cJSON_AddNumberToObject(data.get(),"timeListened",j.listened);
        char* body=cJSON_PrintUnformatted(data.get());int code=0;request(config(),"/api/session/"+url_encode(j.session)+(j.close?"/close":"/sync"),body,"POST",&code);free(body);
        if(code>=200&&code<300){auto cp=load_checkpoint(j.cp.id);if(cp.updated_ms==j.cp.updated_ms&&cp.position==j.cp.position){cp.pending=false;save_checkpoint(cp);}}
        // Failed sessions are not blindly replayed over newer remote progress.
        // The durable pending checkpoint is reconciled when the book next opens.
        sync_active--;
    }
}
struct Book {std::string id,title,session;std::vector<Track> tracks;std::vector<Chapter> chapters;double duration=0,position=0;};
static bool load(Book& b,const std::string& id){
    auto c=config();int code=0;Json detail(cJSON_Parse(request(c,"/api/items/"+id+"?expanded=1","","GET",&code).c_str()));
    if(code!=200||!detail){status("Cannot load book",code==401?"Sign in again on phone":"Check Audiobookshelf connection");return false;}
    auto media=obj(detail.get(),"media");auto files=obj(media,"audioFiles");
    if(!cJSON_IsArray(files)||!cJSON_GetArraySize(files)){status("Unsupported book","No audio files");return false;}
    cJSON* file; cJSON_ArrayForEach(file,files){auto ext=str(obj(file,"metadata"),"ext");auto codec=str(file,"codec");if(ext!=".mp3"&&ext!="mp3"&&codec!="mp3"){status("Unsupported book","This version plays MP3 books");return false;}}
    Json session(cJSON_Parse(request(c,"/api/items/"+id+"/play","{\"mediaPlayer\":\"PaperAudio\",\"supportedMimeTypes\":[\"audio/mpeg\"],\"deviceInfo\":{\"deviceId\":\"paper-audio-esp32\",\"clientName\":\"Paper Audio\",\"clientVersion\":\"0.1.0\"}}","POST",&code).c_str()));
    if(code!=200||!session){status("Cannot start session","Check account and connection");return false;}
    b={};b.id=id;b.title=str(obj(media,"metadata"),"title");b.session=str(session.get(),"id");b.duration=num(session.get(),"duration",num(media,"duration"));
    auto ts=obj(session.get(),"audioTracks");cJSON* t;cJSON_ArrayForEach(t,ts){Track track;auto ref=str(t,"contentUrl");track.url=resolve_url(c.base,ref);track.start=num(t,"startOffset");track.duration=num(t,"duration");track.title=str(t,"title");if(url_origin(track.url)!=url_origin(c.base)){status("Cannot play","Unexpected audio URL origin");return false;}b.tracks.push_back(std::move(track));}
    if(b.tracks.empty()){status("Cannot play","No direct MP3 tracks");return false;}
    std::sort(b.tracks.begin(),b.tracks.end(),[](auto& a,auto& z){return a.start<z.start;});
    cJSON* ch;cJSON_ArrayForEach(ch,obj(media,"chapters")){b.chapters.push_back({str(ch,"title"),num(ch,"start")});}
    if(b.chapters.empty())for(size_t i=0;i<b.tracks.size();i++)b.chapters.push_back({"Track "+std::to_string(i+1),b.tracks[i].start});
    auto progress=obj(detail.get(),"userMediaProgress");
    // Expanded item responses do not always include user progress; use the dedicated API.
    Json pj(cJSON_Parse(request(c,"/api/me/progress/"+id,"","GET",&code).c_str()));if(code==200&&pj)progress=pj.get();
    double remote=num(session.get(),"currentTime");int64_t updated=num(progress,"lastUpdate");auto cp=load_checkpoint(id);b.position=resume_position(cp,remote,updated,b.duration);
    {std::lock_guard<std::mutex> l(state_mutex);state.id=b.id;state.title=b.title;state.duration=b.duration;state.position=b.position;state.chapters=b.chapters;state.controls.view=View::Playing;state.error.clear();}
    set_last_book(id);return true;
}
static void player_task(void*){
    Book book;bool playing=false,opened=false,restore_done=false;size_t track=0;double cursor=0,target=0,listened=0;uint64_t sync_at=monotonic_ms(),sleep_at=0,heartbeat=0;FrameReader reader;
    void* decoder=nullptr;ESP_ERROR_CHECK(esp_mp3_dec_open(nullptr,0,&decoder));uint8_t encoded[1500];int16_t pcm[2304],mono[1152];
    auto checkpoint=[&](bool close=false){if(book.id.empty())return;int64_t updated=wall_ms();if(updated<1700000000000LL)updated=load_checkpoint(book.id).updated_ms+1;sync_job({{book.id,book.position,updated,true},book.session,listened,close});listened=0;sync_at=monotonic_ms();};
    auto seek=[&](double at){if(book.tracks.empty())return;book.position=std::clamp(at,0.0,book.duration);track=track_at(book.tracks,book.position);target=std::max(0.0,book.position-book.tracks[track].start);auto p=book.tracks[track].index.before(target);cursor=p.seconds;reader={};stream->start(config(),book.tracks[track].url,p.byte);esp_mp3_dec_reset(decoder);opened=true;status("Buffering");};
    for(;;){
        Command* raw=nullptr;
        while(xQueueReceive(commands,&raw,0)==pdTRUE){std::unique_ptr<Command> cmd(raw);
            if(!cmd->book.empty()){
                playing=false;stream->stop();speaker().mute();checkpoint(true);
                // Finish the previous session before starting the next one on this device.
                uint64_t until=monotonic_ms()+7000;for(;;){bool empty;{std::lock_guard<std::mutex> l(sync_mutex);empty=jobs.empty()&&sync_active==0;}if(empty||monotonic_ms()>until)break;vTaskDelay(pdMS_TO_TICKS(30));}
                Book next;if(load(next,cmd->book)){book=std::move(next);playing=true;seek(book.position);}else {book={};opened=false;}
            }else if(cmd->action==Action::None&&cmd->value==-1){if(!playing)speaker_test();}
            else if(cmd->action==Action::Shutdown){playing=false;stream->stop();speaker().mute();checkpoint(true);uint64_t until=monotonic_ms()+6500;for(;;){bool empty;{std::lock_guard<std::mutex> l(sync_mutex);empty=jobs.empty()&&sync_active==0;}if(empty||monotonic_ms()>until)break;vTaskDelay(pdMS_TO_TICKS(30));}board_poweroff();}
            else if(cmd->action==Action::Toggle&&!book.id.empty()){playing=!playing;if(playing&&!opened)seek(book.position);if(!playing){speaker().mute();checkpoint();}}
            else if(cmd->action==Action::Back30||cmd->action==Action::Forward30){seek(book.position+(cmd->action==Action::Back30?-30:30));checkpoint();}
            else if(cmd->action==Action::Chapter&&cmd->value>=0&&cmd->value<book.chapters.size()){seek(book.chapters[(size_t)cmd->value].start);checkpoint();}
            else if(cmd->action==Action::Volume){save_volume(cmd->value);}
            else if(cmd->action==Action::Sleep){sleep_at=cmd->value?monotonic_ms()+(uint64_t)cmd->value*60000:0;std::lock_guard<std::mutex> l(state_mutex);state.sleep_minutes=cmd->value;}
        }
        if(!restore_done&&wifi_connected()&&!config().token.empty()){
            restore_done=true;auto id=last_book();if(book.id.empty()&&!id.empty()){if(load(book,id)){playing=false;opened=false;status("Paused");}}
        }
        if(sleep_at&&monotonic_ms()>=sleep_at){sleep_at=0;playing=false;speaker().mute();checkpoint();std::lock_guard<std::mutex> l(state_mutex);state.sleep_minutes=0;}
        {std::lock_guard<std::mutex> l(state_mutex);state.playing=playing;state.position=book.position;}
        if(monotonic_ms()-sync_at>=15000&&!book.id.empty())checkpoint();
        if(monotonic_ms()-heartbeat>30000){heartbeat=monotonic_ms();ESP_LOGI("player","playing=%d position=%.2f free_heap=%lu",playing,book.position,(unsigned long)esp_get_free_heap_size());}
        if(!playing){if(!book.id.empty())status("Paused");vTaskDelay(pdMS_TO_TICKS(30));continue;}
        if(!opened){seek(book.position);continue;}
        uint64_t byte=stream->offset();Mp3Header h;int r=reader.next(encoded,h);
        if(r==0)continue;
        if(r==-2){
            if(track+1<book.tracks.size()){book.position=book.tracks[track+1].start;seek(book.position);checkpoint();}
            else{book.position=book.duration;playing=false;opened=false;stream->stop();speaker().mute();checkpoint(true);status("Finished");}
            continue;
        }
        if(r<0){playing=false;opened=false;speaker().mute();checkpoint();status("Playback stopped",r==-3?"Invalid MP3 data":stream->error());continue;}
        // ID3 bytes may have been skipped; index the actual frame, not the tag.
        byte=stream->offset()-h.bytes;book.tracks[track].index.add(cursor,byte);
        double start=cursor;cursor+=(double)h.samples/h.rate;
        // Scan frame headers quickly until one second before the target, then decode preroll.
        if(cursor<target-1.0)continue;
        esp_audio_dec_in_raw_t in={};in.buffer=encoded;in.len=h.bytes;esp_audio_dec_out_frame_t out={};out.buffer=(uint8_t*)pcm;out.len=sizeof(pcm);esp_audio_dec_info_t info={};
        auto result=esp_mp3_dec_decode(decoder,&in,&out,&info);
        if(result!=ESP_AUDIO_ERR_OK||out.decoded_size==0)continue;
        if(info.bits_per_sample!=16||(info.channel!=1&&info.channel!=2)||!info.sample_rate){playing=false;status("Unsupported MP3 output");continue;}
        size_t count=out.decoded_size/(2*info.channel);if(count>1152){playing=false;status("Invalid decoder output");continue;}
        size_t skip=target>start?std::min(count,(size_t)std::ceil((target-start)*info.sample_rate)):0;
        for(size_t i=skip;i<count;i++)mono[i-skip]=info.channel==2?((int32_t)pcm[2*i]+pcm[2*i+1])/2:pcm[i];
        if(count>skip){auto s=snapshot();if(!speaker().write(mono,count-skip,info.sample_rate,s.volume)){playing=false;status("Speaker error");continue;}
            double elapsed=(double)(count-skip)/info.sample_rate;listened+=elapsed;book.position=std::min(book.duration,book.tracks[track].start+cursor);status("Playing");}
    }
}
void player_start(){
    commands=xQueueCreate(16,sizeof(Command*));stream=new Stream;
    xTaskCreatePinnedToCore([](void*){stream->run();},"audio_network",10240,nullptr,4,nullptr,0);
    xTaskCreatePinnedToCore(player_task,"player",14336,nullptr,6,nullptr,1);
    xTaskCreate(sync_task,"progress",8192,nullptr,2,nullptr);
}
}
