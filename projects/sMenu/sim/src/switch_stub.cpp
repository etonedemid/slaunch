// Implementations for the libnx shim. See sim/include/switch.h for why this
// exists and what it is allowed to fake.

#include <switch.h>

#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

    // ---- simulated console state ------------------------------------------
    std::string g_nickname = "Simulator";
    int         g_battery  = 78;
    bool        g_charging = false;

    bool        g_net_connected = true;
    bool        g_net_ethernet  = false;
    int         g_net_strength  = 3;
    std::string g_net_ssid      = "sLaunch-Test";
    std::string g_net_ip        = "192.168.1.42";
    bool        g_wifi_on       = true;

    std::string g_language = "en-US";

    // ---- shared font -------------------------------------------------------
    std::string        g_font_override;
    std::string        g_font_path;
    std::vector<u8>    g_font_bytes;

    // Where to look for a font to stand in for the console's shared font.
    //
    // The console's own font is Nintendo-licensed and is not in this repo, so
    // the simulator cannot ship it. Drop a dump at the first path below and the
    // simulator matches the console exactly, including text metrics; without it
    // the layout is right but glyph widths differ slightly, so measure spacing
    // on hardware, not here. sim/README.md says the same thing.
    const char *kFontSearch[] = {
        // Always first, so a dumped console font wins wherever it is running.
        "sdmc:/slaunch/sim/font.ttf",
#ifdef _WIN32
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
        "C:/Windows/Fonts/tahoma.ttf",
#else
        // The Windows font directory as WSL sees it, which is usually the only
        // set of TTFs present in a minimal WSL image.
        "/mnt/c/Windows/Fonts/segoeui.ttf",
        "/mnt/c/Windows/Fonts/arial.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
#endif
    };

    bool ReadWholeFile(const char *path, std::vector<u8> &out) {
        FILE *fp = fopen(path, "rb");
        if (!fp) return false;
        fseek(fp, 0, SEEK_END);
        const long n = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        if (n <= 0) { fclose(fp); return false; }
        out.resize((size_t)n);
        const bool ok = fread(out.data(), 1, (size_t)n, fp) == (size_t)n;
        fclose(fp);
        return ok;
    }

}   // namespace

// ---------------------------------------------------------------------------
namespace simsw {
    void SetNickname(const char *name) { if (name) g_nickname = name; }
    void SetBattery(int percent, bool charging) {
        g_battery  = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
        g_charging = charging;
    }
    void SetNetwork(bool connected, bool ethernet, int strength,
                    const char *ssid, const char *ip) {
        g_net_connected = connected;
        g_net_ethernet  = ethernet;
        g_net_strength  = strength < 0 ? 0 : (strength > 3 ? 3 : strength);
        if (ssid) g_net_ssid = ssid;
        if (ip)   g_net_ip   = ip;
    }
    void SetLanguage(const char *code) { if (code) g_language = code; }
    void SetFontPath(const char *path) { if (path) g_font_override = path; }
    const char *FontPath() { return g_font_path.c_str(); }
}

// ---------------------------------------------------------------------------
// Time.
//
// The tick rate is reported as the console's real 19.2 MHz rather than as
// nanoseconds. Several places in the menu divide by the frequency to get
// milliseconds, but a couple of animation constants were tuned against the real
// number, so keeping it identical means the simulator animates at exactly the
// speed hardware does.
extern "C" u64 armGetSystemTickFreq(void) { return 19200000ull; }

extern "C" u64 armGetSystemTick(void) {
    using namespace std::chrono;
    static const steady_clock::time_point base = steady_clock::now();
    const u64 ns = (u64)duration_cast<nanoseconds>(steady_clock::now() - base).count();
    // ns * 19.2e6 / 1e9, done so the multiply cannot overflow after a long run.
    return (ns / 1000ull) * 192ull / 10ull;
}

extern "C" void fatalThrow(Result err) {
    fprintf(stderr, "[sim] fatalThrow(0x%08x) - the console would have crashed here\n",
            (unsigned)err);
    abort();
}

// ---------------------------------------------------------------------------
// Threads. Real std::threads: the worker paths in the menu are where the
// interesting bugs are, and running them single-threaded here would hide them.
extern "C" Result threadCreate(Thread *t, void (*entry)(void *), void *arg,
                               void *stack_mem, size_t stack_sz, int prio, int cpuid) {
    (void)stack_mem; (void)stack_sz; (void)prio; (void)cpuid;
    if (!t || !entry) return SIM_UNSUPPORTED;
    // The entry point is parked here and the thread is not spawned until
    // threadStart, so that stays a real transition: code that creates a thread
    // and forgets to start it does nothing here, exactly as on hardware.
    t->handle = new std::pair<void (*)(void *), void *>(entry, arg);
    return 0;
}

extern "C" Result threadStart(Thread *t) {
    if (!t || !t->handle) return SIM_UNSUPPORTED;
    auto *fn = (std::pair<void (*)(void *), void *> *)t->handle;
    auto *th = new std::thread(fn->first, fn->second);
    delete fn;
    t->handle = th;
    return 0;
}

extern "C" Result threadWaitForExit(Thread *t) {
    if (!t || !t->handle) return SIM_UNSUPPORTED;
    auto *th = (std::thread *)t->handle;
    if (th->joinable()) th->join();
    return 0;
}

extern "C" Result threadClose(Thread *t) {
    if (!t || !t->handle) return 0;
    auto *th = (std::thread *)t->handle;
    if (th->joinable()) th->detach();
    delete th;
    t->handle = nullptr;
    return 0;
}

// Seeded from the clock so a random pick differs between runs, as it does on
// hardware. Not a CSPRNG, and nothing here needs one.
extern "C" u64 randomGet64(void) {
    static u64 s = 0;
    if (!s) s = (u64)std::chrono::steady_clock::now().time_since_epoch().count() | 1ull;
    s ^= s << 13; s ^= s >> 7; s ^= s << 17;      // xorshift64
    return s;
}

extern "C" void svcSleepThread(u64 ns) {
    std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
}

extern "C" u32 hosversionGet(void) { return (18u << 16) | (1u << 8) | 0u; }

// ---------------------------------------------------------------------------
// Kernel introspection: no equivalent on a PC. Failing is correct - the debug
// overlay already has to handle these failing on hardware.
extern "C" Result svcGetInfo(u64 *out, u32, Handle, u64) { if (out) *out = 0; return SIM_UNSUPPORTED; }
extern "C" Result svcGetSystemInfo(u64 *out, u32, Handle, u64) { if (out) *out = 0; return SIM_UNSUPPORTED; }
extern "C" Result svcGetResourceLimitCurrentValue(s64 *out, Handle, u32) { if (out) *out = 0; return SIM_UNSUPPORTED; }
extern "C" Result svcGetResourceLimitLimitValue(s64 *out, Handle, u32) { if (out) *out = 0; return SIM_UNSUPPORTED; }
extern "C" Result svcGetResourceLimitPeakValue(s64 *out, Handle, u32) { if (out) *out = 0; return SIM_UNSUPPORTED; }
extern "C" Result svcCloseHandle(Handle) { return 0; }

// ---------------------------------------------------------------------------
extern "C" Result psmGetBatteryChargePercentage(u32 *out) {
    if (!out) return SIM_UNSUPPORTED;
    *out = (u32)g_battery;
    return 0;
}

extern "C" Result psmGetChargerType(PsmChargerType *out) {
    if (!out) return SIM_UNSUPPORTED;
    *out = g_charging ? PsmChargerType_EnoughPower : PsmChargerType_Unconnected;
    return 0;
}

extern "C" Result setGetSystemLanguage(u64 *out) {
    if (!out) return SIM_UNSUPPORTED;
    u64 v = 0;
    memcpy(&v, g_language.c_str(),
           g_language.size() < 8 ? g_language.size() : 8);
    *out = v;
    return 0;
}

// ---------------------------------------------------------------------------
extern "C" Result plGetSharedFontByType(PlFontData *out, PlSharedFontType type) {
    if (!out) return SIM_UNSUPPORTED;

    if (g_font_bytes.empty()) {
        if (!g_font_override.empty() && ReadWholeFile(g_font_override.c_str(), g_font_bytes))
            g_font_path = g_font_override;
        else
            for (const char *p : kFontSearch)
                if (ReadWholeFile(p, g_font_bytes)) { g_font_path = p; break; }
    }
    if (g_font_bytes.empty()) {
        fprintf(stderr,
                "[sim] no font found. Put a TTF at sdmc:/slaunch/sim/font.ttf\n");
        return SIM_UNSUPPORTED;
    }

    out->address = g_font_bytes.data();
    out->size    = g_font_bytes.size();
    out->type    = type;
    return 0;
}

// ---------------------------------------------------------------------------
// Play statistics, synthesised from the application id. See switch.h for why
// this one is allowed to invent values where the others just fail.
extern "C" Result pdmqryInitialize(void) { return 0; }
extern "C" void   pdmqryExit(void) {}

extern "C" Result pdmqryQueryPlayStatisticsByApplicationId(u64 app_id, bool,
                                                           PdmPlayStatistics *out) {
    if (!out) return SIM_UNSUPPORTED;
    if (app_id == 0) return SIM_UNSUPPORTED;

    // Mix the id so neighbouring ids (which real libraries are full of) do not
    // produce neighbouring playtimes, and every title gets a distinguishable
    // row rather than a column of near-identical numbers.
    u64 h = app_id * 0x9E3779B97F4A7C15ull;
    h ^= h >> 29; h *= 0xBF58476D1CE4E5B9ull; h ^= h >> 32;

    // One title in six has never been launched, so the "-" path stays visible.
    if ((h % 6) == 0) return SIM_UNSUPPORTED;

    memset(out, 0, sizeof(*out));
    out->total_launches = 1 + (h % 120);
    out->playtime       = (h % (400ull * 3600ull)) * 1000000000ull;   // up to 400h
    const u64 now       = (u64)time(nullptr);
    out->last_timestamp_user    = now - (h % (400ull * 24ull * 3600ull));  // <400 days
    out->first_timestamp_user   = out->last_timestamp_user;
    return 0;
}

// ---------------------------------------------------------------------------
// No captured transition frame on a desktop; the menu falls back to black.
extern "C" Result capsscInitialize(void) { return SIM_UNSUPPORTED; }
extern "C" void   capsscExit(void) {}
extern "C" Result capsscCaptureRawImageWithTimeout(void *, size_t, ViLayerStack,
                                                   u64, u64, s64, s64, s64) {
    return SIM_UNSUPPORTED;
}

// ---------------------------------------------------------------------------
// Applet storages: no daemon, so nothing to talk to. See switch.h.
extern "C" Result appletCreateStorage(AppletStorage *, s64) { return SIM_UNSUPPORTED; }
extern "C" Result appletStorageClose(AppletStorage *) { return SIM_UNSUPPORTED; }
extern "C" Result appletStorageWrite(AppletStorage *, s64, const void *, size_t) { return SIM_UNSUPPORTED; }
extern "C" Result appletStorageRead(AppletStorage *, s64, void *, size_t) { return SIM_UNSUPPORTED; }
extern "C" Result appletStorageGetSize(AppletStorage *, s64 *size) { if (size) *size = 0; return SIM_UNSUPPORTED; }
extern "C" Result appletPopInData(AppletStorage *) { return SIM_UNSUPPORTED; }
extern "C" Result appletPushOutData(AppletStorage *) { return SIM_UNSUPPORTED; }
extern "C" Result appletHolderPopOutData(AppletHolder *, AppletStorage *) { return SIM_UNSUPPORTED; }

// ---------------------------------------------------------------------------
extern "C" bool accountUidIsValid(const AccountUid *uid) {
    return uid && (uid->uid[0] != 0 || uid->uid[1] != 0);
}

extern "C" Result accountListAllUsers(AccountUid *uids, s32 max, s32 *actual) {
    if (!uids || max < 1) { if (actual) *actual = 0; return SIM_UNSUPPORTED; }
    uids[0].uid[0] = 0x53494d55ull;   // "SIMU"
    uids[0].uid[1] = 1;
    if (actual) *actual = 1;
    return 0;
}

extern "C" Result accountGetProfile(AccountProfile *out, AccountUid uid) {
    (void)uid;
    if (out) memset(out, 0, sizeof(*out));
    return 0;
}

extern "C" Result accountProfileGet(AccountProfile *p, void *userdata,
                                    AccountProfileBase *base) {
    (void)p; (void)userdata;
    if (!base) return SIM_UNSUPPORTED;
    memset(base, 0, sizeof(*base));
    snprintf(base->nickname, sizeof(base->nickname), "%s", g_nickname.c_str());
    return 0;
}

extern "C" Result accountProfileClose(AccountProfile *p) { (void)p; return 0; }

// ---------------------------------------------------------------------------
extern "C" Result nifmGetCurrentIpAddress(u32 *out) {
    if (!out) return SIM_UNSUPPORTED;
    if (!g_net_connected) { *out = 0; return 0; }
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (sscanf(g_net_ip.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) { *out = 0; return 0; }
    // Low byte first, matching what the menu decodes.
    *out = (u32)(a | (b << 8) | (c << 16) | (d << 24));
    return 0;
}


extern "C" Result nifmGetCurrentNetworkProfile(NifmNetworkProfileData *profile) {
    if (!profile) return SIM_UNSUPPORTED;
    memset(profile, 0, sizeof(*profile));
    if (!g_net_connected) return SIM_UNSUPPORTED;
    if (g_net_ethernet) {
        snprintf(profile->network_name, sizeof(profile->network_name), "Wired");
    } else {
        snprintf(profile->wireless_setting_data.ssid,
                 sizeof(profile->wireless_setting_data.ssid), "%s", g_net_ssid.c_str());
        profile->wireless_setting_data.ssid_len = (u8)g_net_ssid.size();
    }
    return 0;
}

extern "C" Result nifmGetInternetConnectionStatus(NifmInternetConnectionType *type,
                                                  u32 *strength,
                                                  NifmInternetConnectionStatus *status) {
    const bool up = g_net_connected && (g_net_ethernet || g_wifi_on);
    if (type)     *type     = g_net_ethernet ? NifmInternetConnectionType_Ethernet
                                             : NifmInternetConnectionType_WiFi;
    if (strength) *strength = (u32)(up && !g_net_ethernet ? g_net_strength : 0);
    if (status)   *status   = up ? NifmInternetConnectionStatus_Connected
                                 : NifmInternetConnectionStatus_ConnectingUnknown1;
    return 0;
}

extern "C" Result nifmIsWirelessCommunicationEnabled(bool *out) {
    if (!out) return SIM_UNSUPPORTED;
    *out = g_wifi_on;
    return 0;
}

extern "C" Result nifmSetWirelessCommunicationEnabled(bool enable) {
    g_wifi_on = enable;
    return 0;
}
