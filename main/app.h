#pragma once
#include "core.h"
#include <atomic>
#include <mutex>
#include <memory>
#include "cJSON.h"
#include "esp_http_client.h"

namespace paper {
struct JsonDelete { void operator()(cJSON* j)const{cJSON_Delete(j);} };
using Json=std::unique_ptr<cJSON,JsonDelete>;
inline std::string str(cJSON* j,const char* k){auto v=cJSON_GetObjectItemCaseSensitive(j,k);return cJSON_IsString(v)?v->valuestring:"";}
inline double num(cJSON* j,const char* k,double d=0){auto v=cJSON_GetObjectItemCaseSensitive(j,k);return cJSON_IsNumber(v)?v->valuedouble:d;}
inline cJSON* obj(cJSON* j,const char* k){return cJSON_GetObjectItemCaseSensitive(j,k);}
struct Config {std::string ssid,password,base="http://umbrel.local:13378/audiobookshelf",token;};
struct Chapter {std::string title;double start=0;};
struct Snapshot {
    std::string id,title="Paper Audio",status="Starting",ip,setup_ssid,setup_password,error;
    double position=0,duration=0;
    int volume=20,battery=-1,sleep_minutes=0;
    bool playing=false,connected=false,configured=false,shutdown=false;
    Controls controls;
    std::vector<Chapter> chapters;
};
extern std::mutex state_mutex;
extern Snapshot state;
Snapshot snapshot();
void status(const std::string& text,const std::string& error="");
Config config();
void save_config(const Config& c);
Checkpoint load_checkpoint(const std::string& id);
void save_checkpoint(const Checkpoint& cp);
std::string last_book();
void set_last_book(const std::string& id);
void save_volume(int volume);
int load_volume();
void network_start();
bool wifi_connected();
std::string request(const Config& c,const std::string& path,const std::string& body="",const char* method="GET",int* code=nullptr);
void server_start();
int64_t wall_ms();
uint64_t monotonic_ms();
struct Command {Action action=Action::None;double value=0;std::string book;};
bool enqueue(Command c);
void player_start();
void board_start();
void board_poweroff();
class AudioOutput {
public:
    virtual ~AudioOutput()=default;
    virtual bool write(const int16_t* mono,size_t samples,int rate,int volume)=0;
    virtual void mute()=0;
};
AudioOutput& speaker();
void speaker_test();
}
