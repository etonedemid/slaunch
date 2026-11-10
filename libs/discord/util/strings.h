#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Str {

// Decodes one UTF-8 code point and advances p. Invalid bytes decode as U+FFFD.
uint32_t NextCp(const char*& p, const char* end);
void AppendCp(std::string& out, uint32_t cp);
size_t CpCount(std::string_view s);

std::string Format(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
std::string Lower(std::string s);
std::string Trim(std::string_view s);
bool StartsWith(std::string_view s, std::string_view prefix);
bool EndsWith(std::string_view s, std::string_view suffix);
std::vector<std::string> Split(std::string_view s, char sep);
std::string Replace(std::string s, std::string_view from, std::string_view to);
// Collapses newlines into spaces, for one-line previews.
std::string OneLine(std::string_view s);
// Drops trailing partial UTF-8 bytes after a byte-length cut.
std::string Utf8Truncate(std::string_view s, size_t maxBytes);

std::string HumanSize(uint64_t bytes);
std::string UrlEncode(std::string_view s);

// Snowflakes.
uint64_t ParseU64(std::string_view s);
std::string Id(uint64_t id);
int64_t SnowflakeMs(uint64_t id);  // unix epoch milliseconds
uint64_t SnowflakeFromMs(int64_t unixMs);

// Base64 (standard alphabet) and base64url (no padding).
std::string Base64(const uint8_t* data, size_t len);
std::string Base64Url(const uint8_t* data, size_t len);
std::vector<uint8_t> Base64Decode(std::string_view s);  // accepts both alphabets

// JSON string literal with quotes.
std::string Json(std::string_view s);

}  // namespace Str
