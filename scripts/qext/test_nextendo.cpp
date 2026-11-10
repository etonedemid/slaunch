// g++ -std=c++20 -I projects/sMenu/include scripts/qext/test_nextendo.cpp -o /tmp/t && /tmp/t
#include <sl/menu/play/Nextendo.hpp>
#include <cassert>
using namespace sl::menu::play;
int main() {
    std::map<uint64_t, int> m;
    assert(ParseNextendoCounts(R"({"ok":1,"counts":{"0100965017338000":12,"010028600EBDA000": 3},"t":{"x":9}})", m, false));
    assert(m.size() == 2 && m[0x0100965017338000ULL] == 12 && m[0x010028600EBDA000ULL] == 3);
    assert(ParseNextendoCounts(R"({"counts":{"0100965017338000":99,"0100E65002BB8000":1}})", m, true));
    assert(m[0x0100965017338000ULL] == 12 && m[0x0100E65002BB8000ULL] == 1);
    assert(!ParseNextendoCounts("<html>nope</html>", m, false));
    assert(ParseNextendoCounts(R"({"counts":{}})", m, false));

    assert(ParseNextendoGameStats(R"({"online":true,"connected":7})", true) == 7);
    assert(ParseNextendoGameStats(R"({"online":false,"connected":7})", true) == -1);
    assert(ParseNextendoGameStats(R"({"connected": 4})", false) == 4);
    assert(ParseNextendoGameStats(R"({"online":true})", true) == -1);
}
