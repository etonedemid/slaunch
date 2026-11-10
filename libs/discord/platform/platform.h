#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Everything that differs between the Switch build and the desktop/preview build.
namespace Platform {

enum Button : uint32_t {
    BTN_A = 1u << 0,
    BTN_B = 1u << 1,
    BTN_X = 1u << 2,
    BTN_Y = 1u << 3,
    BTN_L = 1u << 4,
    BTN_R = 1u << 5,
    BTN_ZL = 1u << 6,
    BTN_ZR = 1u << 7,
    BTN_PLUS = 1u << 8,
    BTN_MINUS = 1u << 9,
    BTN_UP = 1u << 10,
    BTN_DOWN = 1u << 11,
    BTN_LEFT = 1u << 12,
    BTN_RIGHT = 1u << 13,
    BTN_LSTICK = 1u << 14,
    BTN_RSTICK = 1u << 15,
};

struct Input {
    uint32_t held = 0, down = 0, up = 0;
    float lx = 0, ly = 0, rx = 0, ry = 0;  // -1..1, +y is up
    bool touching = false;
    float tx = 0, ty = 0;  // in 1280x720 screen space
};

struct FontBlob {
    const void* data = nullptr;
    size_t size = 0;
    std::string name;
};

struct LocalTime {
    int year = 1970, month = 1, day = 1, hour = 0, minute = 0, second = 0, wday = 0;
};

bool Init();
void Shutdown();
bool Running();
Input PollInput();

std::string DataDir();                         // ends with '/'
std::string AssetPath(const std::string& rel);  // romfs asset
std::vector<FontBlob> SystemFonts();           // fallback fonts for CJK and symbols

// Shows the system keyboard. Returns false if cancelled.
bool Keyboard(const std::string& guide, const std::string& initial, std::string& out, int maxLen = 2000,
              bool secret = false, bool multiline = false);

int64_t NowMs();
LocalTime ToLocal(int64_t unixMs);
bool NetworkUp();
void Random(void* buf, size_t len);
bool AppletMode();
void SetCpuBoost(bool on);
std::string DeviceName();

}  // namespace Platform
