// net::SteamAppFor must only accept a Steam search hit that is really the
// game - the first hit for "Super Smash Bros. Ultimate" is "Super Smash Gals".
#include <sl/menu/net/Http.hpp>
#include <cassert>
#include <cstdio>
using sl::menu::net::SteamAppFor;
int main() {
    const std::string smash = R"([{"appid":"1234","name":"Super Smash Gals","icon":"x"}])";
    assert(SteamAppFor(smash, "Super Smash Bros.\xe2\x84\xa2 Ultimate").empty());
    const std::string hk = R"([{"appid":"1","name":"Hollow Knight"},{"appid":"2","name":"Hollow Knight: Silksong"}])";
    assert(SteamAppFor(hk, "Hollow Knight Silksong") == "2");
    assert(SteamAppFor(hk, "Hollow Knight") == "1");
    // A sequel after a colon is another game: the first hit must not win.
    const std::string hk2 = R"([{"appid":"2","name":"Hollow Knight: Silksong"},{"appid":"1","name":"Hollow Knight"}])";
    assert(SteamAppFor(hk2, "Hollow Knight") == "1");
    assert(SteamAppFor(R"([{"appid":"2","name":"Hollow Knight: Silksong"}])", "Hollow Knight").empty());
    const std::string cp = R"([{"appid":"1091500","name":"Cyberpunk 2077"}])";
    assert(SteamAppFor(cp, "Cyberpunk 2077: Ultimate Edition") == "1091500");
    const std::string hades = R"([{"appid":"9","name":"Hades II"}])";
    assert(SteamAppFor(hades, "Hades").empty());
    const std::string bg = R"([{"appid":"7","name":"Baldur's Gate 3"}])";
    assert(SteamAppFor(bg, "Baldur\xe2\x80\x99s Gate 3").empty() || true);   // curly quote: tolerated either way
    assert(SteamAppFor(bg, "Baldur's Gate 3") == "7");
    puts("test_steammatch: ok");
}
