#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace paper {
struct Mp3Header { int bytes=0, rate=0, samples=0, channels=0; };
bool mp3_header(const uint8_t* p, Mp3Header& h);
uint64_t id3_size(const uint8_t* p);
struct SeekPoint { double seconds; uint64_t byte; };
class SeekIndex {
    std::vector<SeekPoint> points;
public:
    void add(double seconds, uint64_t byte);
    SeekPoint before(double seconds) const;
    size_t size() const { return points.size(); }
};
struct Track { std::string url, title; double start=0, duration=0; SeekIndex index; };
size_t track_at(const std::vector<Track>& tracks, double seconds);
struct Checkpoint { std::string id; double position=0; int64_t updated_ms=0; bool pending=false; };
double resume_position(const Checkpoint& local, double remote_position, int64_t remote_updated, double duration);
enum class ButtonEvent { None, Tap, Hold };
class Button {
    bool raw=false, stable=false, held=false, armed=false;
    uint64_t changed=0, pressed=0;
    uint32_t hold_ms;
public:
    explicit Button(uint32_t hold=800): hold_ms(hold) {}
    ButtonEvent update(bool down, uint64_t now);
};
enum class View { Playing, Menu, Volume, Chapters, Sleep };
enum class Action { None, Toggle, Back30, Forward30, Volume, Chapter, Sleep, Shutdown };
struct UiResult { Action action=Action::None; int value=0; };
struct Controls {
    View view=View::Playing; int cursor=0, volume=20;
    UiResult input(bool power, ButtonEvent event, int chapters);
};
std::string url_encode(const std::string& s);
std::string url_origin(const std::string& s);
std::string resolve_url(const std::string& base, const std::string& ref);
bool valid_base_url(const std::string& s);
}
