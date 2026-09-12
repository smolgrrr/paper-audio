#include "app.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <ctime>
#include <cstdio>

namespace paper {
std::mutex state_mutex;
Snapshot state;
static std::mutex nvs_mutex;
static std::string get(nvs_handle_t h,const char* key){size_t n=0;if(nvs_get_str(h,key,nullptr,&n)!=ESP_OK)return {};std::string s(n,'\0');nvs_get_str(h,key,s.data(),&n);s.resize(n?n-1:0);return s;}
Snapshot snapshot(){std::lock_guard<std::mutex> l(state_mutex);return state;}
void status(const std::string& text,const std::string& error){std::lock_guard<std::mutex> l(state_mutex);state.status=text;state.error=error;}
uint64_t monotonic_ms(){return esp_timer_get_time()/1000;}
int64_t wall_ms(){struct timespec ts;clock_gettime(CLOCK_REALTIME,&ts);return (int64_t)ts.tv_sec*1000+ts.tv_nsec/1000000;}
Config config(){
    std::lock_guard<std::mutex> l(nvs_mutex);nvs_handle_t h;Config c;
    if(nvs_open("paper_cfg",NVS_READONLY,&h)!=ESP_OK)return c;
    c.ssid=get(h,"ssid");c.password=get(h,"wifi_pass");c.token=get(h,"token");auto b=get(h,"base");if(!b.empty())c.base=b;nvs_close(h);return c;
}
void save_config(const Config& c){
    std::lock_guard<std::mutex> l(nvs_mutex);nvs_handle_t h;ESP_ERROR_CHECK(nvs_open("paper_cfg",NVS_READWRITE,&h));
    ESP_ERROR_CHECK(nvs_set_str(h,"ssid",c.ssid.c_str()));ESP_ERROR_CHECK(nvs_set_str(h,"wifi_pass",c.password.c_str()));
    ESP_ERROR_CHECK(nvs_set_str(h,"base",c.base.c_str()));ESP_ERROR_CHECK(nvs_set_str(h,"token",c.token.c_str()));
    ESP_ERROR_CHECK(nvs_commit(h));nvs_close(h);
}
static std::string cpkey(const std::string& id){uint64_t h=14695981039346656037ULL;for(unsigned char c:id){h^=c;h*=1099511628211ULL;}char b[16];snprintf(b,sizeof(b),"p%014llx",(unsigned long long)(h&0xffffffffffffffULL));return b;}
Checkpoint load_checkpoint(const std::string& id){
    std::lock_guard<std::mutex> l(nvs_mutex);Checkpoint cp;cp.id=id;nvs_handle_t h;
    if(nvs_open("paper_state",NVS_READONLY,&h)!=ESP_OK)return cp;
    auto s=get(h,cpkey(id).c_str());nvs_close(h);Json j(cJSON_Parse(s.c_str()));
    if(j && str(j.get(),"id")==id){cp.position=num(j.get(),"position");cp.updated_ms=num(j.get(),"updated");cp.pending=cJSON_IsTrue(obj(j.get(),"pending"));}return cp;
}
void save_checkpoint(const Checkpoint& cp){
    std::lock_guard<std::mutex> l(nvs_mutex);nvs_handle_t h;if(nvs_open("paper_state",NVS_READWRITE,&h)!=ESP_OK)return;
    Json j(cJSON_CreateObject());cJSON_AddStringToObject(j.get(),"id",cp.id.c_str());cJSON_AddNumberToObject(j.get(),"position",cp.position);cJSON_AddNumberToObject(j.get(),"updated",cp.updated_ms);cJSON_AddBoolToObject(j.get(),"pending",cp.pending);
    char* s=cJSON_PrintUnformatted(j.get());auto e=nvs_set_str(h,cpkey(cp.id).c_str(),s);free(s);if(e==ESP_OK)nvs_commit(h);else ESP_LOGE("checkpoint","NVS write failed: %s",esp_err_to_name(e));nvs_close(h);
}
std::string last_book(){std::lock_guard<std::mutex> l(nvs_mutex);nvs_handle_t h;if(nvs_open("paper_state",NVS_READONLY,&h)!=ESP_OK)return {};auto s=get(h,"last");nvs_close(h);return s;}
void set_last_book(const std::string& id){std::lock_guard<std::mutex> l(nvs_mutex);nvs_handle_t h;if(nvs_open("paper_state",NVS_READWRITE,&h)==ESP_OK){nvs_set_str(h,"last",id.c_str());nvs_commit(h);nvs_close(h);}}
void save_volume(int v){std::lock_guard<std::mutex> l(nvs_mutex);nvs_handle_t h;if(nvs_open("paper_state",NVS_READWRITE,&h)==ESP_OK){nvs_set_i32(h,"volume",v);nvs_commit(h);nvs_close(h);}}
int load_volume(){std::lock_guard<std::mutex> l(nvs_mutex);nvs_handle_t h;int32_t v=20;if(nvs_open("paper_state",NVS_READONLY,&h)==ESP_OK){nvs_get_i32(h,"volume",&v);nvs_close(h);}return std::clamp((int)v,0,100);}
}
extern "C" void app_main(){
    // Never erase NVS automatically: a migration error must not destroy credentials.
    ESP_ERROR_CHECK(nvs_flash_init());
    paper::state.volume=paper::load_volume();paper::state.controls.volume=paper::state.volume;
    paper::board_start();paper::network_start();paper::player_start();paper::server_start();
    ESP_LOGI("paper","Paper Audio ready; configure through the address on the display");
}
