// Host check for the in-game Discord pop-up drawing (projects/sMenu/source/sl/menu/qext/GameToast.cpp).
// Renders each style at a few moments, composited over BACKGROUND (straight alpha,
// as vi is assumed to), into one PNG, and checks a few pixels.
//   c++ -std=c++20 -O1 -I projects/sMenu/include scripts/qext/test_game_toast.cpp \
//       projects/sMenu/source/sl/menu/qext/GameToast.cpp $(pkg-config --cflags --libs sdl3 sdl3-ttf sdl3-image) -o toast-test
//   ./toast-test FONT.ttf BACKGROUND.png AVATAR.png out.png
#include <sl/menu/qext/GameToast.hpp>
#include <SDL3_image/SDL_image.h>
#include <cassert>
#include <cstdio>
#include <vector>

using namespace sl::menu::qext::toast;

// The layer, as vi would put it on the 1280x720 screen `dst` (scaled down from `scale`).
static void Composite(SDL_Surface *dst, const Canvas &cv) {
    for (int y = 0; y < dst->h; y++)
        for (int x = 0; x < dst->w; x++) {
            const float lx = (x + 0.5f - cv.region.x) * cv.scale, ly = (y + 0.5f - cv.region.y) * cv.scale;
            if (lx < 0 || ly < 0 || lx >= cv.w || ly >= cv.h) continue;
            const Uint8 *s = cv.px + (int)ly * cv.pitch + (int)lx * 4;
            Uint8 *d = (Uint8 *)dst->pixels + y * dst->pitch + x * 4;
            const float a = s[3] / 255.0f;
            for (int k = 0; k < 3; k++) d[k] = (Uint8)(s[k] * a + d[k] * (1.0f - a) + 0.5f);
        }
}

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: %s font bg avatar out.png\n", argv[0]); return 2; }
    SDL_Init(0);
    TTF_Init();
    const float scale = 1.5f;   // docked: 1920x1080 logical
    Fonts f;
    f.face[0] = TTF_OpenFont(argv[1], 20.0f * scale);
    assert(f.face[0]);
    f.count = 1;
    SDL_Surface *bg = SDL_ConvertSurface(IMG_Load(argv[2]), SDL_PIXELFORMAT_RGBA32);
    SDL_Surface *av = SDL_ConvertSurface(IMG_Load(argv[3]), SDL_PIXELFORMAT_RGBA32);
    assert(bg && av);
    SDL_Surface *screen = SDL_ScaleSurface(bg, 1280, 720, SDL_SCALEMODE_LINEAR);

    const Look::Style styles[] = { Look::Achievement, Look::Xmb, Look::Card };
    const float times[] = { 0.12f, 0.4f, 2.5f, 4.85f };
    const std::string title = "dragonfruit in #general", text = "are you still on? \xF0\x9F\x98\x80 the raid starts in five minutes, bring the good gear";
    SDL_Surface *sheet = SDL_CreateSurface(640 * 4, 360 * 3, SDL_PIXELFORMAT_RGBA32);
    for (int si = 0; si < 3; si++) {
        Look look;
        look.style = styles[si];
        look.fill = SDL_Color{ 46, 52, 64, 245 };
        for (int ti = 0; ti < 4; ti++) {
            const SDL_Rect R = Region(look.style);
            Canvas cv;
            cv.w = (int)(R.w * scale + 0.999f);
            cv.h = (int)(R.h * scale + 0.999f);
            cv.pitch = cv.w * 4;
            cv.scale = scale;
            cv.region = R;
            std::vector<Uint8> buf((size_t)cv.pitch * cv.h, 0);
            cv.px = buf.data();
            Draw(cv, look, f, title, text, ti == 3 ? nullptr : av, times[ti]);
            // Nothing may land outside the region it asked for: the layer is only that big.
            SDL_Surface *shot = SDL_DuplicateSurface(screen);
            Composite(shot, cv);
            SDL_Rect dst{ ti * 640, si * 360, 640, 360 };
            SDL_BlitSurfaceScaled(shot, nullptr, sheet, &dst, SDL_SCALEMODE_LINEAR);
            SDL_DestroySurface(shot);
            // Mid-way the pop-up is fully on screen and opaque somewhere.
            if (ti == 2) {
                int opaque = 0;
                for (size_t i = 3; i < buf.size(); i += 4) opaque += buf[i] > 200;
                assert(opaque > 1000);
            }
        }
    }
    IMG_SavePNG(sheet, argv[4]);
    printf("ok %s\n", argv[4]);
    return 0;
}
