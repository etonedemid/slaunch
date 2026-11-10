#include "util/log.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>

// Into the menu's boot.log, opened per line: it survives Close(), a crash, and a stop
// for a game, and it is the one log that is known to be readable over MTP.
namespace Log {
namespace {
std::mutex g_mu;
}  // namespace

void Init(const std::string&) {}
void Close() {}

void Write(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lk(g_mu);
    if (FILE* f = fopen("sdmc:/slaunch/boot.log", "a")) {
        fprintf(f, "discord: %s\n", buf);
        fclose(f);
    }
}

}  // namespace Log
