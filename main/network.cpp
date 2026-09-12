#include "app.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_http_server.h"
#include "esp_crt_bundle.h"
#include "esp_random.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "nvs.h"
#include "mdns.h"
#include "freertos/task.h"
#include <cstring>
#include <cstdio>

namespace paper {
static std::atomic<bool> connected{false};
static httpd_handle_t server;
extern const char html_start[] asm("_binary_index_html_start");
extern const char html_end[] asm("_binary_index_html_end");
bool wifi_connected(){return connected;}
static void event(void*,esp_event_base_t base,int32_t id,void* data){
    if(base==WIFI_EVENT&&id==WIFI_EVENT_STA_START){auto c=config();if(!c.ssid.empty())esp_wifi_connect();}
    if(base==WIFI_EVENT&&id==WIFI_EVENT_STA_DISCONNECTED){connected=false;{std::lock_guard<std::mutex> l(state_mutex);state.connected=false;state.ip="192.168.4.1";if(state.id.empty())state.status="Connect to setup Wi-Fi";}}
    if(base==IP_EVENT&&id==IP_EVENT_STA_GOT_IP){
        connected=true;auto e=(ip_event_got_ip_t*)data;char ip[24];snprintf(ip,sizeof(ip),IPSTR,IP2STR(&e->ip_info.ip));
        std::lock_guard<std::mutex> l(state_mutex);state.connected=true;state.ip=ip;if(state.id.empty())state.status="Select a book";
    }
}
static void reconnect(void*){for(;;){vTaskDelay(pdMS_TO_TICKS(5000));if(!connected&&!config().ssid.empty())esp_wifi_connect();}}
void network_start(){
    ESP_ERROR_CHECK(esp_netif_init());ESP_ERROR_CHECK(esp_event_loop_create_default());
    auto sta=esp_netif_create_default_wifi_sta();esp_netif_set_hostname(sta,"paper-audio");esp_netif_create_default_wifi_ap();
    wifi_init_config_t init=WIFI_INIT_CONFIG_DEFAULT();ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,ESP_EVENT_ANY_ID,event,nullptr));ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,event,nullptr));
    auto c=config();wifi_config_t old={};esp_wifi_get_config(WIFI_IF_STA,&old);
    if(c.ssid.empty()&&old.sta.ssid[0]){c.ssid=(char*)old.sta.ssid;c.password=(char*)old.sta.password;save_config(c);}
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    uint8_t mac[6];esp_read_mac(mac,ESP_MAC_WIFI_SOFTAP);char name[32];snprintf(name,sizeof(name),"PaperAudio-%02X%02X",mac[4],mac[5]);
    char pass[20]={};nvs_handle_t h;ESP_ERROR_CHECK(nvs_open("paper_setup",NVS_READWRITE,&h));size_t n=sizeof(pass);
    if(nvs_get_str(h,"password",pass,&n)!=ESP_OK){snprintf(pass,sizeof(pass),"%08lx%04lx",(unsigned long)esp_random(),(unsigned long)(esp_random()&65535));nvs_set_str(h,"password",pass);nvs_commit(h);}nvs_close(h);
    wifi_config_t ap={};strncpy((char*)ap.ap.ssid,name,sizeof(ap.ap.ssid)-1);strncpy((char*)ap.ap.password,pass,sizeof(ap.ap.password)-1);ap.ap.ssid_len=strlen(name);ap.ap.channel=1;ap.ap.max_connection=3;ap.ap.authmode=WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP,&ap));
    wifi_config_t wc={};strncpy((char*)wc.sta.ssid,c.ssid.c_str(),sizeof(wc.sta.ssid));strncpy((char*)wc.sta.password,c.password.c_str(),sizeof(wc.sta.password));wc.sta.pmf_cfg.capable=true;ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA,&wc));
    {std::lock_guard<std::mutex> l(state_mutex);state.setup_ssid=name;state.setup_password=pass;state.configured=!c.token.empty();state.ip="192.168.4.1";state.status="Connect to setup Wi-Fi";}
    ESP_ERROR_CHECK(esp_wifi_start());esp_wifi_set_ps(WIFI_PS_NONE);
    mdns_init();mdns_hostname_set("paper-audio");mdns_instance_name_set("Paper Audio");mdns_service_add(nullptr,"_http","_tcp",80,nullptr,0);
    esp_sntp_config_t sc=ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");esp_netif_sntp_init(&sc);
    xTaskCreate(reconnect,"wifi_retry",3072,nullptr,2,nullptr);
}
std::string request(const Config& c,const std::string& path,const std::string& body,const char* method,int* code){
    std::string url=path.rfind("http",0)==0?path:c.base+path;
    if(code)*code=0;if(url_origin(url)!=url_origin(c.base))return {};
    esp_http_client_config_t hc={};hc.url=url.c_str();hc.timeout_ms=6000;hc.crt_bundle_attach=esp_crt_bundle_attach;hc.disable_auto_redirect=true;hc.buffer_size=4096;
    auto h=esp_http_client_init(&hc);if(!h)return {};
    esp_http_client_set_header(h,"Accept","application/json");esp_http_client_set_header(h,"Accept-Encoding","identity");
    if(!c.token.empty()){auto auth="Bearer "+c.token;esp_http_client_set_header(h,"Authorization",auth.c_str());}
    if(!strcmp(method,"POST"))esp_http_client_set_method(h,HTTP_METHOD_POST);
    if(!strcmp(method,"PATCH"))esp_http_client_set_method(h,HTTP_METHOD_PATCH);
    if(!body.empty())esp_http_client_set_header(h,"Content-Type","application/json");
    std::string result;
    if(esp_http_client_open(h,body.size())==ESP_OK){
        size_t sent=0;while(sent<body.size()){int n=esp_http_client_write(h,body.data()+sent,body.size()-sent);if(n<=0)break;sent+=n;}
        if(sent==body.size()&&esp_http_client_fetch_headers(h)>=0){
            int statuscode=esp_http_client_get_status_code(h);if(code)*code=statuscode;
            char buf[2048];int n;while((n=esp_http_client_read(h,buf,sizeof(buf)))>0){if(result.size()+n>256*1024){result.clear();if(code)*code=413;break;}result.append(buf,n);}
        }
    }
    esp_http_client_cleanup(h);return result;
}
static esp_err_t reply(httpd_req_t* r,cJSON* j,const char* statuscode="200 OK"){
    httpd_resp_set_status(r,statuscode);httpd_resp_set_type(r,"application/json");httpd_resp_set_hdr(r,"Cache-Control","no-store");char* data=cJSON_PrintUnformatted(j);auto e=httpd_resp_sendstr(r,data);free(data);return e;
}
static esp_err_t error(httpd_req_t* r,const char* message,const char* code="400 Bad Request"){Json j(cJSON_CreateObject());cJSON_AddStringToObject(j.get(),"error",message);return reply(r,j.get(),code);}
static Json receive(httpd_req_t* r){
    if(r->content_len<=0||r->content_len>4096)return {};
    char guard[8];if(httpd_req_get_hdr_value_str(r,"X-Paper-Request",guard,sizeof(guard))!=ESP_OK||strcmp(guard,"1"))return {};
    std::string b(r->content_len,'\0');size_t got=0;while(got<b.size()){int n=httpd_req_recv(r,b.data()+got,b.size()-got);if(n<=0)return {};got+=n;}return Json(cJSON_ParseWithLength(b.data(),b.size()));
}
static esp_err_t homepage(httpd_req_t* r){httpd_resp_set_type(r,"text/html");httpd_resp_set_hdr(r,"Cache-Control","no-store");httpd_resp_set_hdr(r,"X-Content-Type-Options","nosniff");httpd_resp_set_hdr(r,"Content-Security-Policy","default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; frame-ancestors 'none'; base-uri 'none'");return httpd_resp_send(r,html_start,html_end-html_start-1);}
static esp_err_t get_status(httpd_req_t* r){auto s=snapshot();auto c=config();Json j(cJSON_CreateObject());
    cJSON_AddStringToObject(j.get(),"title",s.title.c_str());cJSON_AddStringToObject(j.get(),"status",s.status.c_str());cJSON_AddStringToObject(j.get(),"error",s.error.c_str());cJSON_AddStringToObject(j.get(),"ip",s.ip.c_str());cJSON_AddStringToObject(j.get(),"ssid",c.ssid.c_str());cJSON_AddStringToObject(j.get(),"base",c.base.c_str());cJSON_AddBoolToObject(j.get(),"configured",s.configured);cJSON_AddBoolToObject(j.get(),"connected",s.connected);cJSON_AddBoolToObject(j.get(),"playing",s.playing);cJSON_AddNumberToObject(j.get(),"position",s.position);cJSON_AddNumberToObject(j.get(),"duration",s.duration);cJSON_AddNumberToObject(j.get(),"volume",s.volume);cJSON_AddNumberToObject(j.get(),"battery",s.battery);return reply(r,j.get());}
static void restart_task(void*){vTaskDelay(pdMS_TO_TICKS(1200));esp_restart();}
static esp_err_t configure(httpd_req_t* r){auto j=receive(r);if(!j)return error(r,"Invalid setup request");auto c=config();
    if(obj(j.get(),"ssid"))c.ssid=str(j.get(),"ssid");if(obj(j.get(),"password"))c.password=str(j.get(),"password");
    if(obj(j.get(),"base"))c.base=str(j.get(),"base");while(!c.base.empty()&&c.base.back()=='/')c.base.pop_back();
    if(c.base.size()>=6&&c.base.substr(c.base.size()-6)=="/login")c.base.resize(c.base.size()-6);
    if(c.ssid.empty()||c.ssid.size()>32||c.password.size()>63||!valid_base_url(c.base))return error(r,"Check Wi-Fi name, password and server URL");
    if(obj(j.get(),"token"))c.token=str(j.get(),"token");if(c.token.size()>4096||c.token.find_first_of("\r\n")!=std::string::npos)return error(r,"Invalid token");
    save_config(c);Json ok(cJSON_CreateObject());cJSON_AddBoolToObject(ok.get(),"saved",true);reply(r,ok.get());xTaskCreate(restart_task,"restart",2048,nullptr,1,nullptr);return ESP_OK;
}
static esp_err_t login(httpd_req_t* r){auto j=receive(r);if(!j)return error(r,"Invalid login request");auto c=config();c.token.clear();int code=0;
    char* body=cJSON_PrintUnformatted(j.get());auto data=request(c,"/login",body,"POST",&code);free(body);Json resp(cJSON_Parse(data.c_str()));
    if(code!=200||!resp)return error(r,"Cannot sign in. Check server address, Wi-Fi and account.","502 Bad Gateway");
    c.token=str(obj(resp.get(),"user"),"accessToken");if(c.token.empty())c.token=str(obj(resp.get(),"user"),"token");if(c.token.empty())c.token=str(resp.get(),"accessToken");
    if(c.token.empty())return error(r,"This server requires an API key. Enter it in setup.");save_config(c);{std::lock_guard<std::mutex> l(state_mutex);state.configured=true;state.status="Select a book";}
    Json ok(cJSON_CreateObject());cJSON_AddBoolToObject(ok.get(),"signedIn",true);return reply(r,ok.get());
}
static std::string query(httpd_req_t* r,const char* key){char q[512],out[256];if(httpd_req_get_url_query_str(r,q,sizeof(q))!=ESP_OK)return {};if(httpd_query_key_value(q,key,out,sizeof(out))!=ESP_OK)return {};return out;}
static esp_err_t libraries(httpd_req_t* r){auto c=config();int code=0;auto s=request(c,"/api/libraries","","GET",&code);Json j(cJSON_Parse(s.c_str()));if(code!=200||!j)return error(r,"Audiobookshelf unavailable or authentication expired. Open setup to reconnect.","502 Bad Gateway");return reply(r,j.get());}
static esp_err_t books(httpd_req_t* r){auto id=query(r,"library");if(id.empty()||id.size()>80||id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")!=std::string::npos)return error(r,"Invalid library");int page=std::clamp(atoi(query(r,"page").c_str()),0,100000);
    int code=0;auto s=request(config(),"/api/libraries/"+id+"/items?limit=12&page="+std::to_string(page)+"&sort=media.metadata.title","","GET",&code);Json j(cJSON_Parse(s.c_str()));if(code!=200||!j)return error(r,"Could not load books","502 Bad Gateway");return reply(r,j.get());}
static esp_err_t select_book(httpd_req_t* r){auto j=receive(r);if(!j)return error(r,"Invalid selection");auto id=str(j.get(),"id");if(id.empty()||id.size()>80||id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")!=std::string::npos)return error(r,"Invalid book");if(!enqueue({Action::None,0,id}))return error(r,"Player busy","503 Service Unavailable");Json ok(cJSON_CreateObject());cJSON_AddBoolToObject(ok.get(),"selected",true);return reply(r,ok.get(),"202 Accepted");}
static esp_err_t diagnostic(httpd_req_t* r){auto j=receive(r);if(!j)return error(r,"Invalid request");if(snapshot().playing)return error(r,"Pause playback first");if(!enqueue({Action::None,-1,{}}))return error(r,"Player busy");Json ok(cJSON_CreateObject());cJSON_AddBoolToObject(ok.get(),"queued",true);return reply(r,ok.get());}
void server_start(){
    httpd_config_t c=HTTPD_DEFAULT_CONFIG();c.stack_size=10240;c.max_uri_handlers=12;c.lru_purge_enable=true;c.recv_wait_timeout=5;c.send_wait_timeout=5;ESP_ERROR_CHECK(httpd_start(&server,&c));
    struct Route{const char* path;httpd_method_t method;esp_err_t(*fn)(httpd_req_t*);};
    Route routes[]={{"/",HTTP_GET,homepage},{"/api/status",HTTP_GET,get_status},{"/api/config",HTTP_POST,configure},{"/api/login",HTTP_POST,login},{"/api/libraries",HTTP_GET,libraries},{"/api/books",HTTP_GET,books},{"/api/select",HTTP_POST,select_book},{"/api/speaker-test",HTTP_POST,diagnostic}};
    for(auto& r:routes){httpd_uri_t u={};u.uri=r.path;u.method=r.method;u.handler=r.fn;ESP_ERROR_CHECK(httpd_register_uri_handler(server,&u));}
}
}
