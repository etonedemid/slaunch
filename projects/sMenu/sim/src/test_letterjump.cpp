// Mirror of Menu::XmbInitial / XmbLetterJump against a plain vector, so the
// jump arithmetic is checked without standing up a Menu and a GPU.
#include <string>
#include <vector>
#include <cassert>
#include <cstdio>
#include <algorithm>

static char Initial(const std::string &name) {
    if (name.empty()) return '#';
    const unsigned char c = (unsigned char)name[0];
    if (c >= 'a' && c <= 'z') return (char)(c - 32);
    if (c >= 'A' && c <= 'Z') return (char)c;
    return '#';
}
static int Jump(const std::vector<std::string> &v, int cur, int dir) {
    const int n = (int)v.size();
    if (n == 0) return 0;
    cur = std::min(std::max(0, cur), n - 1);
    const char here = Initial(v[cur]);
    if (dir > 0) {
        for (int i = cur + 1; i < n; i++) if (Initial(v[i]) != here) return i;
        return n - 1;
    }
    int start = cur;
    while (start > 0 && Initial(v[start - 1]) == here) start--;
    if (start < cur) return start;
    if (start == 0) return 0;
    int j = start - 1;
    const char prev = Initial(v[j]);
    while (j > 0 && Initial(v[j - 1]) == prev) j--;
    return j;
}
int main() {
    std::vector<std::string> v = {
        "3 Ninjas", "16 Tiles",            // 0,1  '#'
        "Aladdin", "Altered Beast",        // 2,3  'A'
        "Barbie", "Batman", "Bubba",       // 4,5,6 'B'
        "Comix Zone",                      // 7    'C'
        "sonic spinball", "Sonic 2",       // 8,9  'S' (case-insensitive)
    };
    assert(Initial("3 Ninjas") == '#' && Initial("[BIOS] x") == '#');
    assert(Initial("sonic") == 'S' && Initial("Sonic") == 'S');

    // Forward: first entry of each next letter.
    assert(Jump(v, 0, +1) == 2);   // '#' -> A
    assert(Jump(v, 2, +1) == 4);   // A   -> B
    assert(Jump(v, 5, +1) == 7);   // mid-B -> C
    assert(Jump(v, 7, +1) == 8);   // C   -> S
    assert(Jump(v, 8, +1) == 9);   // last letter: clamp to end, still moves
    assert(Jump(v, 9, +1) == 9);   // already at end: stays

    // Backward lands on the FIRST entry of the previous letter, so repeated
    // presses walk letters rather than crawling inside one.
    assert(Jump(v, 8, -1) == 7);   // S -> C
    assert(Jump(v, 7, -1) == 4);   // C -> first B, not last
    assert(Jump(v, 6, -1) == 4);   // mid-B -> first B (not past it)
    assert(Jump(v, 5, -1) == 4);   // also mid-B -> first B
    assert(Jump(v, 4, -1) == 2);   // first B -> first A
    assert(Jump(v, 2, -1) == 0);   // A -> first '#'
    assert(Jump(v, 0, -1) == 0);   // at start: stays

    // Repeated back presses must keep moving, never stick.
    int i = 9, guard = 0;
    while (i > 0 && guard++ < 100) { int p = Jump(v, i, -1); assert(p < i); i = p; }
    assert(i == 0 && guard < 100);

    // Unsorted column (recently-played Games): must still step, never stick.
    std::vector<std::string> u = {"Zelda", "Mario", "Zelda 2", "Animal Crossing"};
    for (int c = 0; c < (int)u.size() - 1; c++) assert(Jump(u, c, +1) > c);

    // Single entry, and empty.
    std::vector<std::string> one = {"Only"}, none;
    assert(Jump(one, 0, +1) == 0 && Jump(one, 0, -1) == 0);
    assert(Jump(none, 0, +1) == 0 && Jump(none, 0, -1) == 0);

    printf("letter jump: all cases pass\n");
    return 0;
}
