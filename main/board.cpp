#include "app.h"
#include "epaper_driver_bsp.h"
#include "font8x8_basic.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_sleep.h"
#include "esp_log.h"
#include "freertos/task.h"
#include <cmath>

namespace paper {
static i2c_master_bus_handle_t bus;
static adc_oneshot_unit_handle_t adc;
static adc_cali_handle_t cal;
static std::atomic<bool> display_stopped{false};
class Speaker final:public AudioOutput {
    esp_codec_dev_handle_t dev=nullptr;int rate=0;
public:
    void init(){
        i2c_master_bus_config_t bc={};bc.i2c_port=I2C_NUM_0;bc.sda_io_num=GPIO_NUM_47;bc.scl_io_num=GPIO_NUM_48;bc.clk_source=I2C_CLK_SRC_DEFAULT;bc.glitch_ignore_cnt=7;bc.flags.enable_internal_pullup=true;
        ESP_ERROR_CHECK(i2c_new_master_bus(&bc,&bus));
        i2s_chan_handle_t tx=nullptr,rx=nullptr;
        i2s_chan_config_t cc=I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0,I2S_ROLE_MASTER);cc.dma_desc_num=4;cc.dma_frame_num=128;cc.auto_clear=true;
        ESP_ERROR_CHECK(i2s_new_channel(&cc,&tx,&rx));
        i2s_std_config_t sc={};sc.clk_cfg=I2S_STD_CLK_DEFAULT_CONFIG(44100);sc.slot_cfg=I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,I2S_SLOT_MODE_STEREO);
        sc.gpio_cfg.mclk=GPIO_NUM_14;sc.gpio_cfg.bclk=GPIO_NUM_15;sc.gpio_cfg.ws=GPIO_NUM_38;sc.gpio_cfg.dout=GPIO_NUM_45;sc.gpio_cfg.din=GPIO_NUM_16;
        ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx,&sc));ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx,&sc));
        audio_codec_i2s_cfg_t dc={};dc.port=I2S_NUM_0;dc.tx_handle=tx;dc.rx_handle=rx;
        auto data=audio_codec_new_i2s_data(&dc);
        audio_codec_i2c_cfg_t ic={};ic.port=I2C_NUM_0;ic.addr=ES8311_CODEC_DEFAULT_ADDR;ic.bus_handle=bus;
        auto ctrl=audio_codec_new_i2c_ctrl(&ic);auto gpio=audio_codec_new_gpio();
        es8311_codec_cfg_t ec={};ec.ctrl_if=ctrl;ec.gpio_if=gpio;ec.codec_mode=ESP_CODEC_DEV_WORK_MODE_DAC;ec.pa_pin=GPIO_NUM_46;ec.use_mclk=true;ec.hw_gain.pa_voltage=5.0;ec.hw_gain.codec_dac_voltage=3.3;
        auto codec=es8311_codec_new(&ec);esp_codec_dev_cfg_t cfg={};cfg.dev_type=ESP_CODEC_DEV_TYPE_OUT;cfg.codec_if=codec;cfg.data_if=data;
        dev=esp_codec_dev_new(&cfg);assert(dev);
    }
    bool write(const int16_t* mono,size_t samples,int sr,int volume) override {
        if(rate!=sr){if(rate)esp_codec_dev_close(dev);esp_codec_dev_sample_info_t f={};f.sample_rate=sr;f.channel=2;f.bits_per_sample=16;if(esp_codec_dev_open(dev,&f)!=ESP_CODEC_DEV_OK)return false;rate=sr;}
        // Mono in both I2S slots: the ES8311 is a mono DAC; no channel is dropped.
        int16_t stereo[2304];if(samples>1152)return false;
        for(size_t i=0;i<samples;i++)stereo[2*i]=stereo[2*i+1]=mono[i];
        esp_codec_dev_set_out_vol(dev,volume);esp_codec_dev_set_out_mute(dev,volume==0);
        return esp_codec_dev_write(dev,stereo,samples*4)==ESP_CODEC_DEV_OK;
    }
    void mute() override {if(dev)esp_codec_dev_set_out_mute(dev,true);}
};
static Speaker output;
AudioOutput& speaker(){return output;}
void speaker_test(){int16_t tone[441];for(int i=0;i<441;i++)tone[i]=int16_t(1800*std::sin(2*3.141592653589793*440*i/44100));for(int i=0;i<25;i++)output.write(tone,441,44100,15);output.mute();}
static void text(epaper_driver_display& d,int x,int y,const std::string& s,int scale=1,int maxlines=1){
    int sx=x,line=0;
    for(unsigned char c:s){
        if(c>=128)continue;
        if(c=='\n'||x+8*scale>198){x=sx;y+=10*scale;if(++line>=maxlines)return;if(c=='\n')continue;}
        for(int row=0;row<8;row++)for(int col=0;col<8;col++)if(font8x8_basic[c][row]&(1<<col))for(int a=0;a<scale;a++)for(int b=0;b<scale;b++)d.EPD_DrawColorPixel(x+col*scale+a,y+row*scale+b,0);
        x+=8*scale;
    }
}
static std::string clocktext(double seconds){char b[32];int s=std::max(0,(int)seconds);snprintf(b,sizeof(b),"%d:%02d:%02d",s/3600,(s/60)%60,s%60);return b;}
static std::vector<std::string> lines(const Snapshot& s){
    std::vector<std::string> l;
    if(!s.configured || !s.connected){
        l={"PAPER AUDIO",s.status,"Wi-Fi: "+s.setup_ssid,"Key: "+s.setup_password,"Setup: 192.168.4.1",s.ip.empty()?"":("LAN: "+s.ip),s.error};
        if(!s.configured)return l;
    }
    if(s.controls.view==View::Playing){
        l={s.title,clocktext(s.position)+" / "+clocktext(s.duration),s.status,"Volume "+std::to_string(s.volume)+"%  Bat "+(s.battery<0?"--":std::to_string(s.battery)+"%")};
        if(!s.chapters.empty()){size_t n=0;for(size_t i=0;i<s.chapters.size();i++)if(s.chapters[i].start<=s.position)n=i;l.push_back(s.chapters[n].title);}
        if(s.sleep_minutes)l.push_back("Sleep: "+std::to_string(s.sleep_minutes)+" min");
        if(!s.error.empty())l.push_back(s.error);
        if(s.id.empty())l.push_back("Select book on phone");
        l.push_back(s.ip);l.push_back("BOOT Play/Pause");l.push_back("PWR Menu; hold Off");
    }else if(s.controls.view==View::Volume){l={"VOLUME",std::to_string(s.volume)+"%","PWR +5   BOOT -5","Hold BOOT: back"};}
    else{
        std::vector<std::string> options;
        if(s.controls.view==View::Menu)options={"Back 30 seconds","Forward 30 seconds","Volume","Chapters","Sleep timer","Return to playback"};
        if(s.controls.view==View::Sleep)options={"Off","15 minutes","30 minutes","60 minutes"};
        if(s.controls.view==View::Chapters){for(auto& c:s.chapters)options.push_back(c.title);if(options.empty())options.push_back("No book selected");}
        l.push_back(s.controls.view==View::Menu?"PLAYBACK MENU":s.controls.view==View::Sleep?"SLEEP TIMER":"CHAPTERS");
        int first=(s.controls.cursor/6)*6;for(int i=first;i<std::min(first+6,(int)options.size());i++)l.push_back((i==s.controls.cursor?"> ":"  ")+options[i]);
        l.push_back("PWR Next  BOOT Select");l.push_back("Hold BOOT: back");
    }
    return l;
}
static void display_task(void*){
    custom_lcd_spi_t cfg={11,10,9,8,13,12,SPI2_HOST,5000};epaper_driver_display d(200,200,cfg);d.EPD_Init();d.EPD_Clear();d.EPD_DisplayPartBaseImage();d.EPD_Init_Partial();
    std::vector<std::string> previous;uint64_t last=0;int refresh=0;
    for(;;){
        auto s=snapshot();auto ls=lines(s);
        if(s.shutdown){d.EPD_Clear();text(d,4,70,"Good night",2);d.EPD_DisplayPart();display_stopped=true;vTaskDelete(nullptr);}
        // Coalesce fast menu changes; elapsed-time-only refreshes every 15 seconds.
        bool urgent=ls!=previous;
        if(urgent && monotonic_ms()-last>=500){
            bool just_clock=previous.size()==ls.size() && ls.size()>1;
            for(size_t i=0;i<ls.size()&&just_clock;i++)if(i!=1&&ls[i]!=previous[i])just_clock=false;
            if(!just_clock||monotonic_ms()-last>=15000){
                d.EPD_Clear();int y=4;for(size_t i=0;i<ls.size()&&y<188;i++){text(d,4,y,ls[i],1,i==0?2:1);y+=(i==0?26:17);}
                if(++refresh%40==0){d.EPD_Init();d.EPD_DisplayPartBaseImage();d.EPD_Init_Partial();}else d.EPD_DisplayPart();previous=ls;last=monotonic_ms();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
static void buttons_task(void*){
    Button boot(800),power(2000);uint64_t battery_at=0;int low_count=0;
    for(;;){
        auto now=monotonic_ms();ButtonEvent events[]={boot.update(gpio_get_level(GPIO_NUM_0)==0,now),power.update(gpio_get_level(GPIO_NUM_18)==0,now)};
        for(int i=0;i<2;i++)if(events[i]!=ButtonEvent::None){
            UiResult r;{std::lock_guard<std::mutex> l(state_mutex);r=state.controls.input(i==1,events[i],state.chapters.size());if(r.action==Action::Volume)state.volume=r.value;}
            if(r.action!=Action::None)enqueue({r.action,(double)r.value,{}});
        }
        if(now-battery_at>15000){battery_at=now;int raw=0,mv=0;adc_oneshot_read(adc,ADC_CHANNEL_3,&raw);
            if(cal&&adc_cali_raw_to_voltage(cal,raw,&mv)==ESP_OK){mv*=2;int pct=std::clamp((mv-3300)*100/900,0,100);{std::lock_guard<std::mutex> l(state_mutex);state.battery=mv>2000?pct:-1;}
                if(mv>2500&&mv<3300)low_count++;else low_count=0;if(low_count==3)enqueue({Action::Shutdown,0,{}});}
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
void board_poweroff(){
    output.mute();{std::lock_guard<std::mutex> l(state_mutex);state.shutdown=true;state.playing=false;}
    for(int i=0;i<40&&!display_stopped;i++)vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(GPIO_NUM_42,1);gpio_set_level(GPIO_NUM_6,1);
    // Wait for the power button to release or the board could immediately restart.
    while(!gpio_get_level(GPIO_NUM_18))vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(GPIO_NUM_17,0);vTaskDelay(pdMS_TO_TICKS(100));
    // USB power remains present: deep sleep until PWR is pressed.
    esp_sleep_enable_ext1_wakeup(1ULL<<18,ESP_EXT1_WAKEUP_ALL_LOW);esp_deep_sleep_start();
}
void board_start(){
    gpio_config_t g={};g.pin_bit_mask=(1ULL<<17)|(1ULL<<42)|(1ULL<<6);g.mode=GPIO_MODE_OUTPUT;ESP_ERROR_CHECK(gpio_config(&g));
    gpio_set_level(GPIO_NUM_17,1);gpio_set_level(GPIO_NUM_42,0);gpio_set_level(GPIO_NUM_6,0);
    g.pin_bit_mask=(1ULL<<0)|(1ULL<<18);g.mode=GPIO_MODE_INPUT;g.pull_up_en=GPIO_PULLUP_ENABLE;ESP_ERROR_CHECK(gpio_config(&g));
    adc_oneshot_unit_init_cfg_t ac={};ac.unit_id=ADC_UNIT_1;ESP_ERROR_CHECK(adc_oneshot_new_unit(&ac,&adc));adc_oneshot_chan_cfg_t ch={};ch.bitwidth=ADC_BITWIDTH_DEFAULT;ch.atten=ADC_ATTEN_DB_12;ESP_ERROR_CHECK(adc_oneshot_config_channel(adc,ADC_CHANNEL_3,&ch));
    adc_cali_curve_fitting_config_t ca={};ca.unit_id=ADC_UNIT_1;ca.chan=ADC_CHANNEL_3;ca.atten=ADC_ATTEN_DB_12;ca.bitwidth=ADC_BITWIDTH_DEFAULT;adc_cali_create_scheme_curve_fitting(&ca,&cal);
    output.init();
    xTaskCreatePinnedToCore(display_task,"display",6144,nullptr,2,nullptr,0);
    xTaskCreatePinnedToCore(buttons_task,"buttons",4096,nullptr,5,nullptr,0);
}
}
