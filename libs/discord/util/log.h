#pragma once

#include <string>

// A tiny thread-safe logger. On the Switch the file lives next to the settings,
// so a crash or a failed login can be diagnosed from the SD card.
namespace Log {

void Init(const std::string& path);
void Close();
void Write(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace Log

#define LOG(...) Log::Write(__VA_ARGS__)
