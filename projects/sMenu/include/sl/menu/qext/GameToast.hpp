#pragma once
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <functional>
#include <string>

// Discord pop-ups over a running game. The loader stops calling the menu while
// a game has the screen, so these are drawn by a thread of their own into a
// small vi layer stacked above everything (as Tesla overlays are), in software:
// the menu's renderer is not running. One layer per pop-up, gone when it ends.
namespace sl::menu::qext::toast {

    // Which of the menu's banners to look like (Menu::DrawDiscordBanner).
    struct Look {
        enum Style { Card, Xmb, Achievement } style = Card;
        SDL_Color fill{ 40, 40, 48, 245 };      // Card: the theme's
        SDL_Color accent{ 0, 170, 255, 255 };
        SDL_Color title{ 255, 255, 255, 255 };
        SDL_Color dim{ 190, 190, 190, 255 };
        SDL_Color disc{ 104, 176, 38, 255 };    // Achievement: the disc
    };

    struct Item { std::string title, text, avatarUrl; };

    // Starts the pop-up thread. `next` is polled (on that thread) for the next
    // message to show; `sound` runs as one appears. Both run while the menu does
    // not, and Stop() joins before it runs again.
    void Start(const Look &look, std::function<bool(Item &)> next, std::function<void()> sound);
    void Stop();
    // The console is about to sleep: take any pop-up down and show none for a
    // while (vi powers down under it).
    void Hold();

    // ---- the drawing, without the Switch: scripts/qext/test_game_toast.cpp ----
    constexpr float kSecs = 5.0f;   // as long as the menu's banner

    // The fonts, one size (the menu's FontSize::Small at this scale). Extra
    // faces are tried for characters the first lacks; none of them: dropped.
    struct Fonts { TTF_Font *face[6] = {}; int count = 0; };

    // A buffer covering `region` (1280x720 design units) at `scale` pixels per unit.
    struct Canvas {
        Uint8 *px = nullptr;   // RGBA, straight alpha, cleared to 0 by the caller
        int w = 0, h = 0, pitch = 0;
        float scale = 1.0f;
        SDL_Rect region{};
    };

    // Where each style needs its layer, in design units.
    SDL_Rect Region(Look::Style style);
    // Draws the pop-up `secs` after it appeared. `avatar`: RGBA32 or null.
    // Returns false while it holds still (no need to present again).
    bool Draw(Canvas &cv, const Look &look, const Fonts &fonts, const std::string &title,
              const std::string &text, SDL_Surface *avatar, float secs);
}
