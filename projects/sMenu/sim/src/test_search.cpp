// Mirror of Menu::ContainsFold, the name matcher behind search. Checked here
// rather than through Menu, which would need a GPU and a library to stand up.
#include <string>
#include <cassert>
#include <cstdio>

static bool ContainsFold(const std::string &hay, const std::string &needle) {
    if (needle.empty()) return true;
    if (hay.size() < needle.size()) return false;
    auto lower = [](unsigned char c) {
        return (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
    };
    for (size_t i = 0; i + needle.size() <= hay.size(); i++) {
        size_t k = 0;
        while (k < needle.size() &&
               lower((unsigned char)hay[i + k]) == lower((unsigned char)needle[k])) k++;
        if (k == needle.size()) return true;
    }
    return false;
}

int main() {
    // Case-insensitive both ways.
    assert(ContainsFold("Sonic The Hedgehog 2", "sonic"));
    assert(ContainsFold("sonic the hedgehog 2", "SONIC"));
    assert(ContainsFold("Sonic The Hedgehog 2", "HeDgE"));

    // Substring anywhere, not just a prefix - "mario" has to find
    // "Super Mario World", which is the whole point for a ROM set.
    assert(ContainsFold("Super Mario World", "mario"));
    assert(ContainsFold("Super Mario World", "world"));
    assert(ContainsFold("Super Mario World", "r Mar"));

    // Non-matches.
    assert(!ContainsFold("Super Mario World", "zelda"));
    assert(!ContainsFold("abc", "abcd"));          // needle longer than hay
    assert(!ContainsFold("", "a"));

    // Empty needle matches everything: clearing the box must restore the list,
    // not empty it.
    assert(ContainsFold("anything", ""));
    assert(ContainsFold("", ""));

    // Exact-length match, and the last possible offset.
    assert(ContainsFold("abc", "abc"));
    assert(ContainsFold("xxabc", "abc"));

    // A near miss that shares a prefix must not match - catches a loop that
    // forgets to reset its comparison index.
    assert(!ContainsFold("ababab", "ababc"));
    assert(ContainsFold("ababab", "babab"));

    // UTF-8 passes through byte for byte: typed as spelled, it matches.
    assert(ContainsFold("Pokémon Rouge", "Pokémon"));
    assert(ContainsFold("Pokémon Rouge", "rouge"));
    assert(!ContainsFold("Pokemon Red", "Pokémon"));   // folding is ASCII-only

    // Names a real set is full of.
    assert(ContainsFold("Sonic The Hedgehog 2 (World) (Rev A) [!]", "(world)"));
    assert(ContainsFold("[BIOS] X'Eye (USA)", "bios"));

    printf("search matcher: all cases pass\n");
    return 0;
}
