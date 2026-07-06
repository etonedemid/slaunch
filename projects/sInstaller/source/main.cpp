// sInstaller - sLaunch installer homebrew NRO
// Downloads the latest sLaunch release from GitHub and installs it.
// Uses libcurl over HTTPS (mbedtls). Text-only UI.

#include <switch.h>
#include <curl/curl.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdarg>
#include <sys/stat.h>
#include <string>
#include <vector>
#include <functional>

// ---------------------------------------------------------------------------
// ANSI helpers (same as sMenu)
#define ESC        "\x1b["
#define RESET      ESC "0m"
#define BOLD       ESC "1m"
#define DIM        ESC "2m"
#define FG_RED     ESC "31m"
#define FG_GREEN   ESC "32m"
#define FG_YELLOW  ESC "33m"
#define FG_CYAN    ESC "36m"
#define FG_WHITE   ESC "37m"
#define FG_BRIGHT_WHITE ESC "97m"
#define FG_BRIGHT_YELLOW ESC "93m"
#define FG_BRIGHT_GREEN  ESC "92m"
#define FG_BRIGHT_RED    ESC "91m"
#define BG_BLUE    ESC "44m"
#define CLEAR      ESC "2J" ESC "H"
#define CLEAR_LINE ESC "2K"

static constexpr const char *GH_OWNER = "etonedemid";
static constexpr const char *GH_REPO  = "slaunch";
static constexpr const char *GH_API   = "https://api.github.com";

// Destination paths on SD card
static constexpr const char *DST_SSYSTEM = "sdmc:/atmosphere/contents/0100000000001000/exefs/main";
static constexpr const char *DST_SMENU   = "sdmc:/slaunch/bin/sMenu/main";
static constexpr const char *DST_SMENU_DIR = "sdmc:/slaunch/bin/sMenu";

// ---------------------------------------------------------------------------
// libcurl write callback - appends to a std::string
static size_t CurlWriteString(void *ptr, size_t size, size_t nmemb, void *userdata) {
    auto *buf = static_cast<std::string*>(userdata);
    buf->append(static_cast<const char*>(ptr), size * nmemb);
    return size * nmemb;
}

// libcurl write callback - writes to a FILE*
static size_t CurlWriteFile(void *ptr, size_t size, size_t nmemb, void *userdata) {
    return fwrite(ptr, size, nmemb, static_cast<FILE*>(userdata));
}

// Progress callback
struct ProgressCtx {
    std::function<void(double, double)> cb;
};
static int CurlProgress(void *userdata, curl_off_t dltotal, curl_off_t dlnow,
                         curl_off_t, curl_off_t) {
    auto *ctx = static_cast<ProgressCtx*>(userdata);
    if (ctx->cb && dltotal > 0)
        ctx->cb(static_cast<double>(dlnow), static_cast<double>(dltotal));
    return 0;
}

// ---------------------------------------------------------------------------
// Tiny JSON value extractor - no external library needed.
// Finds the first occurrence of "key":"value" or "key":value in a JSON string.
static std::string JsonExtract(const std::string &json, const char *key) {
    std::string needle = std::string("\"") + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return "";

    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return "";

    // Skip whitespace
    while (pos < json.size() && (json[pos] == ':' || json[pos] == ' ')) pos++;

    if (json[pos] == '"') {
        // String value
        pos++;
        auto end = json.find('"', pos);
        if (end == std::string::npos) return "";
        return json.substr(pos, end - pos);
    } else {
        // Number / bool value
        auto end = json.find_first_of(",}\n", pos);
        if (end == std::string::npos) end = json.size();
        return json.substr(pos, end - pos);
    }
}

// Extract all values for a repeated key (e.g., "browser_download_url" in assets array)
static std::vector<std::string> JsonExtractAll(const std::string &json, const char *key) {
    std::vector<std::string> results;
    std::string needle = std::string("\"") + key + "\"";
    size_t search_from = 0;

    while (true) {
        auto pos = json.find(needle, search_from);
        if (pos == std::string::npos) break;
        search_from = pos + needle.size();

        pos = json.find(':', search_from);
        if (pos == std::string::npos) break;
        search_from = pos + 1;

        while (search_from < json.size() && json[search_from] == ' ') search_from++;
        if (search_from >= json.size()) break;

        if (json[search_from] == '"') {
            search_from++;
            auto end = json.find('"', search_from);
            if (end == std::string::npos) break;
            results.push_back(json.substr(search_from, end - search_from));
            search_from = end + 1;
        }
    }
    return results;
}

// ---------------------------------------------------------------------------
// UI helpers

static int g_row = 4; // current log row

static void Header() {
    printf(CLEAR);
    printf(BG_BLUE BOLD FG_BRIGHT_WHITE);
    // Fill header row
    for (int i = 0; i < 160; i++) putchar(' ');
    printf(ESC "1;1H"); // move to row 1 col 1
    printf("  sLaunch Installer");
    // Right side
    printf(ESC "1;130H" "github.com/etonedemid/slaunch" RESET);

    printf(ESC "2;1H" DIM FG_WHITE);
    for (int i = 0; i < 160; i++) putchar('-');
    printf(RESET);
    g_row = 4;
}

static void Log(const char *color, const char *prefix, const char *fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    printf(ESC "%d;1H" CLEAR_LINE, g_row);
    printf("%s%s%s %s\n" RESET, color, prefix, RESET, buf);
    g_row++;
    consoleUpdate(nullptr);
}

#define LOG_INFO(...)  Log(FG_CYAN,          " INFO ", __VA_ARGS__)
#define LOG_OK(...)    Log(FG_BRIGHT_GREEN,  "  OK  ", __VA_ARGS__)
#define LOG_WARN(...)  Log(FG_BRIGHT_YELLOW, " WARN ", __VA_ARGS__)
#define LOG_ERR(...)   Log(FG_BRIGHT_RED,    " ERR  ", __VA_ARGS__)

static void ProgressBar(int row, double done, double total, const char *label) {
    constexpr int BarWidth = 50;
    int filled = (total > 0) ? (int)(done / total * BarWidth) : 0;
    if (filled > BarWidth) filled = BarWidth;

    printf(ESC "%d;1H" CLEAR_LINE, row);
    printf(FG_CYAN "  %s " RESET "[", label);
    for (int i = 0; i < filled; i++) putchar('#');
    for (int i = filled; i < BarWidth; i++) putchar('.');
    printf("]  %3.0f%%" RESET, total > 0 ? (done / total * 100.0) : 0.0);
    consoleUpdate(nullptr);
}

static void Footer(const char *msg) {
    printf(ESC "44;1H" DIM FG_WHITE);
    for (int i = 0; i < 160; i++) putchar('-');
    printf(ESC "45;1H" CLEAR_LINE "%s" RESET, msg);
    consoleUpdate(nullptr);
}

// ---------------------------------------------------------------------------
// HTTP helpers

static CURL *InitCurl() {
    CURL *curl = curl_easy_init();
    if (!curl) return nullptr;
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "sLaunch-Installer/0.1");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L); // Switch doesn't have CA store
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    return curl;
}

static bool HttpGet(const char *url, std::string &out) {
    CURL *curl = InitCurl();
    if (!curl) return false;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);

    CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return rc == CURLE_OK;
}

static bool HttpDownload(const char *url, const char *dst_path,
                          int progress_row, const char *label) {
    // Ensure parent directory exists
    {
        char dir[FS_MAX_PATH];
        strncpy(dir, dst_path, sizeof(dir) - 1);
        char *slash = strrchr(dir, '/');
        if (slash) {
            *slash = '\0';
            // mkdir -p equivalent
            for (char *p = dir + 1; *p; p++) {
                if (*p == '/') {
                    *p = '\0';
                    mkdir(dir, 0777);
                    *p = '/';
                }
            }
            mkdir(dir, 0777);
        }
    }

    FILE *fp = fopen(dst_path, "wb");
    if (!fp) return false;

    CURL *curl = InitCurl();
    if (!curl) { fclose(fp); return false; }

    ProgressCtx pctx;
    pctx.cb = [&](double done, double total) {
        ProgressBar(progress_row, done, total, label);
    };

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteFile);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, CurlProgress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &pctx);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

    CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    fclose(fp);

    return rc == CURLE_OK;
}

// ---------------------------------------------------------------------------
// Core install logic

struct ReleaseInfo {
    std::string tag;
    std::string ssystem_url;
    std::string smenu_url;
};

static bool FetchLatestRelease(ReleaseInfo &out) {
    char url[512];
    snprintf(url, sizeof(url), "%s/repos/%s/%s/releases/latest",
             GH_API, GH_OWNER, GH_REPO);

    LOG_INFO("Fetching release info...");
    std::string body;
    if (!HttpGet(url, body)) {
        LOG_ERR("Failed to reach GitHub API");
        return false;
    }

    out.tag = JsonExtract(body, "tag_name");
    if (out.tag.empty()) {
        LOG_ERR("No releases found on %s/%s", GH_OWNER, GH_REPO);
        return false;
    }

    LOG_OK("Found release: %s", out.tag.c_str());

    // Find download URLs for our two assets
    auto urls = JsonExtractAll(body, "browser_download_url");
    for (auto &u : urls) {
        if (u.find("sSystem.nso") != std::string::npos ||
            u.find("sSystem_main") != std::string::npos)
            out.ssystem_url = u;
        else if (u.find("sMenu.nso") != std::string::npos ||
                 u.find("sMenu_main") != std::string::npos)
            out.smenu_url = u;
    }

    if (out.ssystem_url.empty() || out.smenu_url.empty()) {
        LOG_ERR("Release assets not found (expected sSystem.nso + sMenu.nso)");
        return false;
    }

    return true;
}

static bool Install(const ReleaseInfo &rel) {
    LOG_INFO("Installing sSystem -> %s", DST_SSYSTEM);
    if (!HttpDownload(rel.ssystem_url.c_str(), DST_SSYSTEM, g_row, "sSystem.nso")) {
        LOG_ERR("Failed to download sSystem");
        return false;
    }
    g_row += 2;
    LOG_OK("sSystem installed");

    LOG_INFO("Installing sMenu -> %s", DST_SMENU);
    if (!HttpDownload(rel.smenu_url.c_str(), DST_SMENU, g_row, "sMenu.nso  ")) {
        LOG_ERR("Failed to download sMenu");
        return false;
    }
    g_row += 2;
    LOG_OK("sMenu installed");

    return true;
}

// ---------------------------------------------------------------------------
extern "C" void __appInit() {
    smInitialize();
    fsInitialize();
    fsdevMountSdmc();
    appletInitialize();
    hidInitialize();
    socketInitializeDefault();
    nifmInitialize(NifmServiceType_User);
    curl_global_init(CURL_GLOBAL_ALL);
}

extern "C" void __appExit() {
    curl_global_cleanup();
    nifmExit();
    socketExit();
    hidExit();
    appletExit();
    fsdevUnmountAll();
    fsExit();
    smExit();
}

// ---------------------------------------------------------------------------
int main() {
    consoleInit(nullptr);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    Header();
    Footer(" A: Install    B: Exit");

    LOG_INFO("sLaunch Installer v0.1.0");
    LOG_INFO("Repo: github.com/%s/%s", GH_OWNER, GH_REPO);

    // Wait for user confirmation
    printf(ESC "%d;1H" FG_BRIGHT_YELLOW
           "  Press A to install the latest release, or B to exit." RESET, g_row + 1);
    consoleUpdate(nullptr);

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad);
        if (down & HidNpadButton_A) break;
        if (down & HidNpadButton_B) {
            consoleExit(nullptr);
            return 0;
        }
        svcSleepThread(16'666'666ULL);
    }

    g_row += 3;
    printf(ESC "%d;1H" FG_BRIGHT_WHITE BOLD "  Installing sLaunch..." RESET, g_row);
    g_row += 2;
    consoleUpdate(nullptr);

    // Ensure network is up
    LOG_INFO("Waiting for network...");
    {
        NifmInternetConnectionStatus status;
        int attempts = 0;
        while (attempts++ < 30) {
            u32 strength = 0;
            NifmInternetConnectionType type;
            nifmGetInternetConnectionStatus(&type, &strength, &status);
            if (status == NifmInternetConnectionStatus_Connected) break;
            svcSleepThread(500'000'000ULL);
        }
        NifmInternetConnectionType type;
        u32 strength = 0;
        nifmGetInternetConnectionStatus(&type, &strength, &status);
        if (status != NifmInternetConnectionStatus_Connected) {
            LOG_ERR("No internet connection. Connect to WiFi and try again.");
            goto done;
        }
        LOG_OK("Network ready");
    }

    {
        ReleaseInfo rel;
        if (!FetchLatestRelease(rel)) goto done;
        if (!Install(rel)) goto done;

        g_row++;
        printf(ESC "%d;1H" BOLD FG_BRIGHT_GREEN
               "  Installation complete! Reboot to start sLaunch." RESET, g_row);
        Footer(" Installation complete. Press B to exit.");
    }

done:
    // Wait for B to exit
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_B) break;
        svcSleepThread(16'666'666ULL);
    }

    consoleExit(nullptr);
    return 0;
}
