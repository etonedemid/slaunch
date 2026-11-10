#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

// Players online per title, as nextendo.network's own pages count them (the same
// sources CardsNX reads). Pure parsing, tested by scripts/qext/test_nextendo.cpp.
namespace sl::menu::play {

    // {"counts":{"0100...":12,...}}. Existing entries win when only_new is set.
    inline bool ParseNextendoCounts(const std::string &body, std::map<uint64_t, int> &out, bool only_new) {
        size_t at = body.find("\"counts\"");
        if (at == std::string::npos) return false;
        at = body.find('{', at);
        if (at == std::string::npos) return false;
        const size_t end = body.find('}', at);
        if (end == std::string::npos) return false;
        at++;
        while (true) {
            const size_t q0 = body.find('"', at);
            if (q0 == std::string::npos || q0 > end) return true;
            const size_t q1 = body.find('"', q0 + 1);
            if (q1 == std::string::npos || q1 > end) return true;
            const size_t colon = body.find(':', q1);
            if (colon == std::string::npos || colon > end) return true;
            const uint64_t id = strtoull(body.c_str() + q0 + 1, nullptr, 16);
            char *num_end = nullptr;
            const long n = strtol(body.c_str() + colon + 1, &num_end, 10);
            if (id && num_end != body.c_str() + colon + 1 && (!only_new || !out.count(id)))
                out[id] = (int)n;
            at = colon + 1;
        }
    }

    // A game's own server: {"online":true,"connected":12}. -1 when it has no count,
    // or when needs_online is set and the server says it is down.
    inline int ParseNextendoGameStats(const std::string &body, bool needs_online) {
        if (needs_online && body.find("\"online\":true") == std::string::npos &&
            body.find("\"online\": true") == std::string::npos) return -1;
        size_t at = body.find("\"connected\"");
        if (at == std::string::npos) return -1;
        at = body.find(':', at);
        if (at == std::string::npos) return -1;
        char *end = nullptr;
        const long n = strtol(body.c_str() + at + 1, &end, 10);
        return end == body.c_str() + at + 1 ? -1 : (int)n;
    }

    struct NextendoGame { const char *path; uint64_t id; bool needs_online; };
    inline constexpr NextendoGame kNextendoGames[] = {
        { "/game-stats/sm3dw",       0x010028600EBDA000ULL, false },
        { "/game-stats/jamboree",    0x0100965017338000ULL, true  },
        { "/game-stats/stardew",     0x0100E65002BB8000ULL, true  },
        { "/game-stats/kirby",       0x0100A8E016236000ULL, true  },
        { "/game-stats/justdance22", 0x0100EA6014BB8000ULL, true  },
        { "/game-stats/uno-count",   0x0100B6E012EBE000ULL, false },
    };
}
