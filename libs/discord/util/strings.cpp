#include "util/strings.h"

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace Str {

uint32_t NextCp(const char*& p, const char* end) {
    const unsigned char c = (unsigned char)*p;
    if (c < 0x80) {
        p++;
        return c;
    }
    int n = 0;
    uint32_t cp = 0;
    if ((c & 0xE0) == 0xC0) { n = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { n = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { n = 3; cp = c & 0x07; }
    else { p++; return 0xFFFD; }
    if (end - p <= n) { p = end; return 0xFFFD; }
    for (int i = 1; i <= n; i++) {
        const unsigned char cc = (unsigned char)p[i];
        if ((cc & 0xC0) != 0x80) { p += i; return 0xFFFD; }
        cp = (cp << 6) | (cc & 0x3F);
    }
    p += n + 1;
    return cp;
}

void AppendCp(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

size_t CpCount(std::string_view s) {
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) n++;
    return n;
}

std::string Format(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < (int)sizeof(buf)) return std::string(buf, n > 0 ? n : 0);
    std::string out(n + 1, '\0');
    va_start(ap, fmt);
    vsnprintf(out.data(), out.size(), fmt, ap);
    va_end(ap);
    out.resize(n);
    return out;
}

std::string Lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string Trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return std::string(s.substr(a, b - a));
}

bool StartsWith(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool EndsWith(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::vector<std::string> Split(std::string_view s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); i++) {
        if (i == s.size() || s[i] == sep) {
            out.emplace_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

std::string Replace(std::string s, std::string_view from, std::string_view to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

std::string OneLine(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    bool space = false;
    for (char c : s) {
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        if (c == ' ') {
            if (space) continue;
            space = true;
        } else {
            space = false;
        }
        out += c;
    }
    return Trim(out);
}

std::string Utf8Truncate(std::string_view s, size_t maxBytes) {
    if (s.size() <= maxBytes) return std::string(s);
    size_t n = maxBytes;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    return std::string(s.substr(0, n));
}

std::string HumanSize(uint64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB"};
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024 && u < 3) {
        v /= 1024;
        u++;
    }
    return u == 0 ? Format("%llu B", (unsigned long long)bytes) : Format("%.1f %s", v, units[u]);
}

std::string UrlEncode(std::string_view s) {
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

uint64_t ParseU64(std::string_view s) {
    uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') break;
        v = v * 10 + (uint64_t)(c - '0');
    }
    return v;
}

std::string Id(uint64_t id) { return std::to_string(id); }

int64_t SnowflakeMs(uint64_t id) { return (int64_t)(id >> 22) + 1420070400000LL; }

uint64_t SnowflakeFromMs(int64_t unixMs) {
    if (unixMs < 1420070400000LL) return 0;
    return (uint64_t)(unixMs - 1420070400000LL) << 22;
}

static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64(const uint8_t* d, size_t len) {
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)d[i] << 16;
        if (i + 1 < len) v |= (uint32_t)d[i + 1] << 8;
        if (i + 2 < len) v |= d[i + 2];
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += i + 1 < len ? kB64[(v >> 6) & 63] : '=';
        out += i + 2 < len ? kB64[v & 63] : '=';
    }
    return out;
}

std::string Base64Url(const uint8_t* d, size_t len) {
    std::string s = Base64(d, len);
    while (!s.empty() && s.back() == '=') s.pop_back();
    for (auto& c : s) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    return s;
}

std::vector<uint8_t> Base64Decode(std::string_view s) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+' || c == '-') return 62;
        if (c == '/' || c == '_') return 63;
        return -1;
    };
    std::vector<uint8_t> out;
    out.reserve(s.size() * 3 / 4);
    uint32_t acc = 0;
    int bits = 0;
    for (char c : s) {
        int v = val(c);
        if (v < 0) continue;  // skips '=', whitespace
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((uint8_t)((acc >> bits) & 0xFF));
        }
    }
    return out;
}

std::string Json(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    out += '"';
    return out;
}

}  // namespace Str
