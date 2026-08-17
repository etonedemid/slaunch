// libnx shim for the desktop simulator.
//
// The point of this file is that NOTHING in the menu changes to run on a PC.
// sim/ compiles the real Menu.cpp, Gfx.cpp, Theme.cpp and the rest, unmodified,
// against this header instead of libnx's switch.h - so what you see in the
// simulator is the code that ships, not a second implementation of it that can
// drift.
//
// Two things make that possible:
//
//  - Everything here is either a type the menu passes around without
//    interpreting, or a call whose answer the menu only displays (battery,
//    firmware, nickname, network). Nothing the menu's *layout* depends on is
//    faked.
//  - "sdmc:/..." paths are left completely alone. Linux allows a colon in a
//    filename, so a directory literally named "sdmc:" in the working directory
//    makes every existing path in the menu resolve to the simulated card with
//    no path rewriting anywhere. See sim/README.md.
//
// Anything a simulator genuinely cannot answer returns a failure Result, which
// is the same path the menu already takes on hardware when a service is
// unavailable - so the stubs exercise real code, not a special case.

#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>

// ---------------------------------------------------------------------------
// Scalar types
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;

typedef u32 Result;
typedef u32 Handle;

#ifndef BIT
#define BIT(n) (1u << (n))
#endif

#define R_SUCCEEDED(res) ((res) == 0)
#define R_FAILED(res)    ((res) != 0)
#define MAKERESULT(mod, desc) ((((mod) & 0x1FF)) | ((desc) & 0x1FFF) << 9)

// Generic "this simulator cannot answer that". Callers treat it exactly as they
// treat a service failure on hardware.
#define SIM_UNSUPPORTED MAKERESULT(360, 1)

// libnx's own result module and the one error code sCommon builds a Result
// from, plus the account list bound the menu sizes an array with.
#define Module_Libnx        345
#define LibnxError_BadInput 12
#define ACC_USER_LIST_SIZE  8

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Time. The menu drives every animation off these two, so they must be a real
// monotonic clock or nothing moves.
u64 armGetSystemTick(void);
u64 armGetSystemTickFreq(void);

// ---------------------------------------------------------------------------
// Fatal. On hardware this never returns; here it prints and aborts, which is
// what you want when the simulator hits a path the console would have died on.
void fatalThrow(Result err);

// ---------------------------------------------------------------------------
// Threads. The menu creates real worker threads (app scan, deferred init, cover
// fetch), and those are worth keeping real - they are where the races live.
typedef struct {
    void *handle;
} Thread;

Result threadCreate(Thread *t, void (*entry)(void *), void *arg, void *stack_mem,
                    size_t stack_sz, int prio, int cpuid);
Result threadStart(Thread *t);
Result threadWaitForExit(Thread *t);
Result threadClose(Thread *t);

void svcSleepThread(u64 ns);

// Random. The menu uses this for the random-game pick and the welcome message,
// and both want to actually differ between runs.
u64 randomGet64(void);

// ---------------------------------------------------------------------------
// Kernel introspection, used only by the debug overlay. The ids are named so
// Debug.cpp compiles; the calls all fail, so the overlay shows its
// service-unavailable path - which is itself a screen worth being able to look
// at, and is what the console shows when these are denied.
#define CUR_PROCESS_HANDLE ((Handle)0xFFFF8001)
#define INVALID_HANDLE     ((Handle)0)

typedef enum {
    LimitableResource_Memory          = 0,
    LimitableResource_Threads         = 1,
    LimitableResource_Events          = 2,
    LimitableResource_TransferMemories= 3,
    LimitableResource_Sessions        = 4,
} LimitableResource;

typedef enum {
    InfoType_TotalMemorySize             = 6,
    InfoType_UsedMemorySize              = 7,
    InfoType_HeapRegionSize              = 5,
    InfoType_ProgramId                   = 18,
    InfoType_ResourceLimit               = 12,
    InfoType_FreeThreadCount             = 24,
    InfoType_TotalNonSystemMemorySize    = 21,
    InfoType_UsedNonSystemMemorySize     = 22,
} InfoType;

typedef enum {
    SystemInfoType_TotalPhysicalMemorySize = 0,
    SystemInfoType_UsedPhysicalMemorySize  = 1,
} SystemInfoType;

// Firmware version, reported as the console's so the About screen and the
// overlay have a plausible string rather than 0.0.0.
u32 hosversionGet(void);
#define HOSVER_MAJOR(v) (((v) >> 16) & 0xFF)
#define HOSVER_MINOR(v) (((v) >>  8) & 0xFF)
#define HOSVER_MICRO(v) ( (v)        & 0xFF)

// ---------------------------------------------------------------------------
// Kernel introspection, used only by the debug overlay. All of these fail, so
// the overlay shows its "unavailable" path - which is itself worth being able
// to look at.
Result svcGetInfo(u64 *out, u32 id, Handle h, u64 sub);
Result svcGetSystemInfo(u64 *out, u32 id, Handle h, u64 sub);
Result svcGetResourceLimitCurrentValue(s64 *out, Handle h, u32 which);
Result svcGetResourceLimitLimitValue(s64 *out, Handle h, u32 which);
Result svcGetResourceLimitPeakValue(s64 *out, Handle h, u32 which);
Result svcCloseHandle(Handle h);

// ---------------------------------------------------------------------------
// Power. Configurable at runtime so screenshots can be taken at a chosen
// battery level instead of whatever the host happens to report.
typedef enum {
    PsmChargerType_Unconnected      = 0,
    PsmChargerType_EnoughPower      = 1,
    PsmChargerType_LowPower         = 2,
    PsmChargerType_NotSupported     = 3,
} PsmChargerType;

Result psmGetBatteryChargePercentage(u32 *out);
Result psmGetChargerType(PsmChargerType *out);

// ---------------------------------------------------------------------------
// System settings
Result setGetSystemLanguage(u64 *out);

// ---------------------------------------------------------------------------
// Shared font (pl). Real font bytes, loaded from disk - see switch_stub.cpp for
// the search order. Text rendering is the whole point of the simulator, so this
// is one of the few stubs that has to do actual work.
typedef enum {
    PlSharedFontType_Standard            = 0,
    PlSharedFontType_ChineseSimplified   = 1,
    PlSharedFontType_ExtChineseSimplified= 2,
    PlSharedFontType_ChineseTraditional  = 3,
    PlSharedFontType_KO                  = 4,
    PlSharedFontType_NintendoExt         = 5,
    PlSharedFontType_Total               = 6,
} PlSharedFontType;

typedef struct {
    void *address;
    size_t size;
    PlSharedFontType type;
} PlFontData;

Result plGetSharedFontByType(PlFontData *out, PlSharedFontType type);

// ---------------------------------------------------------------------------
// Accounts
typedef struct { u64 uid[2]; } AccountUid;
typedef struct { u8 _opaque[0x10]; } AccountProfile;

typedef struct {
    AccountUid uid;
    u64        last_edit_timestamp;
    char       nickname[0x20];
    u8         _pad[0x8];
} AccountProfileBase;

bool   accountUidIsValid(const AccountUid *uid);
Result accountListAllUsers(AccountUid *uids, s32 max, s32 *actual);
Result accountGetProfile(AccountProfile *out, AccountUid uid);
Result accountProfileGet(AccountProfile *p, void *userdata, AccountProfileBase *base);
Result accountProfileClose(AccountProfile *p);

// ---------------------------------------------------------------------------
// Network (nifm). Answers come from a small simulated state so the Network
// screen has something to lay out; the Wi-Fi toggle really does flip it.
typedef enum {
    NifmInternetConnectionType_WiFi     = 1,
    NifmInternetConnectionType_Ethernet = 2,
} NifmInternetConnectionType;

typedef enum {
    NifmInternetConnectionStatus_ConnectingUnknown1 = 0,
    NifmInternetConnectionStatus_ConnectingUnknown2 = 1,
    NifmInternetConnectionStatus_ConnectingUnknown3 = 2,
    NifmInternetConnectionStatus_ConnectingUnknown4 = 3,
    NifmInternetConnectionStatus_Connected          = 4,
} NifmInternetConnectionStatus;

typedef struct { u8 uuid[0x10]; } Uuid;

typedef struct {
    u8   ssid_len;
    char ssid[0x21];
    u8   unk_x22;
    u8   pad;
    u32  unk_x24;
    u32  unk_x28;
    u8   passphrase[0x41];
    u8   pad2[0x3];
} NifmWirelessSettingData;

typedef struct { u8 _opaque[0xC0]; } NifmIpSettingData;

typedef struct {
    Uuid                    uuid;
    char                    network_name[0x40];
    u32                     unk_x50;
    u32                     unk_x54;
    u8                      unk_x58;
    u8                      unk_x59;
    u8                      pad[2];
    NifmWirelessSettingData wireless_setting_data;
    NifmIpSettingData       ip_setting_data;
} NifmNetworkProfileData;

Result nifmGetCurrentIpAddress(u32 *out);
Result nifmGetCurrentNetworkProfile(NifmNetworkProfileData *profile);
Result nifmGetInternetConnectionStatus(NifmInternetConnectionType *type,
                                       u32 *strength,
                                       NifmInternetConnectionStatus *status);
Result nifmIsWirelessCommunicationEnabled(bool *out);
Result nifmSetWirelessCommunicationEnabled(bool enable);

// ---------------------------------------------------------------------------
// Play statistics (pdm). Unlike the rest of this header these answers are
// invented rather than merely unavailable, because playtime is one of the few
// service values the menu's LAYOUT depends on: it feeds the XMB sublabel, the
// Recently-played and Most-played sorts, and the length of those strings. With
// everything zeroed those paths cannot be looked at in the simulator at all.
//
// The numbers are derived from the application id, so they are stable across
// runs and across machines - the same library always produces the same ordering
// and the same screenshots. They are not your real playtimes; see sim/README.md.
typedef struct {
    u64 total_launches;
    u64 first_timestamp_user;
    u64 first_timestamp_network;
    u64 last_timestamp_user;
    u64 last_timestamp_network;
    u64 playtime;                 // nanoseconds
} PdmPlayStatistics;

Result pdmqryInitialize(void);
void   pdmqryExit(void);
Result pdmqryQueryPlayStatisticsByApplicationId(u64 app_id, bool by_network,
                                                PdmPlayStatistics *out);

// ---------------------------------------------------------------------------
// Screen capture. On hardware the system keeps the last frame from before an
// applet transition, which is what the menu dissolves in from. There is no
// equivalent here, so this fails and the menu takes its fallback path - a fade
// up from black - which is worth being able to look at in the simulator anyway.
typedef enum {
    ViLayerStack_Default             = 0,
    ViLayerStack_Lcd                 = 1,
    ViLayerStack_Screenshot          = 2,
    ViLayerStack_Recording           = 3,
    ViLayerStack_LastFrame           = 4,
    ViLayerStack_Arbitrary           = 5,
    ViLayerStack_ApplicationForDebug = 6,
    ViLayerStack_Null                = 10,
} ViLayerStack;

Result capsscInitialize(void);
void   capsscExit(void);
Result capsscCaptureRawImageWithTimeout(void *buf, size_t size, ViLayerStack stack,
                                        u64 width, u64 height, s64 buffer_count,
                                        s64 buffer_index, s64 timeout);

// ---------------------------------------------------------------------------
// Applet storages. These carry the SMI protocol between the menu and the
// sSystem daemon, and there is no daemon here - so every one of them fails.
// sl::smi::StorageWriter already handles a failed create (it marks itself
// invalid), which means the menu's command path compiles and runs and simply
// reports that it could not send. That is the correct simulation of "no daemon
// is listening", and it is why the simulator can afford to log actions rather
// than pretend to perform them.
#define FS_MAX_PATH 0x301

typedef struct { void *_unused; } AppletStorage;
typedef struct { void *_unused; } AppletHolder;

Result appletCreateStorage(AppletStorage *s, s64 size);
Result appletStorageClose(AppletStorage *s);
Result appletStorageWrite(AppletStorage *s, s64 off, const void *buf, size_t size);
Result appletStorageRead(AppletStorage *s, s64 off, void *buf, size_t size);
Result appletStorageGetSize(AppletStorage *s, s64 *size);
Result appletPopInData(AppletStorage *s);
Result appletPushOutData(AppletStorage *s);
Result appletHolderPopOutData(AppletHolder *h, AppletStorage *s);

// ---------------------------------------------------------------------------
// NS application types. Declared so sl/os/Applications.hpp compiles; the
// simulator feeds the menu its entry list from the transplanted card cache
// instead of calling NS, so none of these are ever populated here.
typedef struct {
    u64 application_id;
    u8  type;
    u8  unk_x09;
    u8  unk_x0a[6];
    u8  unk_x10;
    u8  unk_x11[7];
} NsApplicationRecord;

typedef struct {
    u64 application_id;
    u32 unk_x08;
    u32 flags;
    u8  unk_x10[0x10];
} NsApplicationView;

typedef struct {
    char name[0x200];
    char author[0x100];
} NacpLanguageEntry;

typedef struct {
    NacpLanguageEntry lang[16];
    u8                _rest[0x3000 - sizeof(NacpLanguageEntry) * 16];
} NacpStruct;

typedef struct {
    NacpStruct nacp;
    u8         icon[0x20000];
} NsApplicationControlData;

#ifdef __cplusplus
}   // extern "C"
#endif

// ---------------------------------------------------------------------------
// Simulator-only controls. Nothing in the menu sees these; the simulator host
// (main_sim.cpp) uses them to set up the fake console state.
#ifdef __cplusplus
namespace simsw {
    // Nickname reported by accountProfileGet.
    void SetNickname(const char *name);
    // Battery percentage and whether a charger is attached.
    void SetBattery(int percent, bool charging);
    // Network state reported by the nifm calls.
    void SetNetwork(bool connected, bool ethernet, int strength,
                    const char *ssid, const char *ip);
    // Console language, as the 8-byte code setGetSystemLanguage returns.
    void SetLanguage(const char *code);
    // Absolute path of the TTF served as the shared font, or nullptr to use the
    // built-in search order. Must be set before Gfx::Init.
    void SetFontPath(const char *path);
    // Resolved font path, for logging. Empty until plGetSharedFontByType runs.
    const char *FontPath();
}
#endif
