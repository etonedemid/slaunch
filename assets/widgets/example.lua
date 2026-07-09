-- Example sLaunch custom widget.
-- Copy to  sdmc:/slaunch/widgets/  on the SD card; every .lua file there is
-- loaded as a widget on the home screen.
--
-- The engine calls two globals:
--   update()          - on a background thread (may use net_*); no drawing here
--   render(x, y, w)    - each frame on the main thread; draw only, return the
--                        pixel height you used so the next widget stacks below.
--
-- Engine API (see sMenu/include/sl/menu/widgets/IWidget.hpp):
--   Drawing (render only):
--     gfx_fill_rect(x,y,w,h, r,g,b,a)
--     gfx_text(x,y,text)                    small white text
--     gfx_text_ex(size,x,y,text, r,g,b)     size 0=Small 1=Normal 2=Large 3=Title
--     gfx_text_width(text) -> px
--     gfx_image(path,x,y,w,h) -> bool       draw a jpg/png from the SD card
--     theme_color(name) -> r,g,b            "bg" "fg" "dim" "accent" "title"
--     screen_width() / screen_height()
--   Networking (update only):
--     net_get(url) -> string | nil
--     net_download(url, path) -> bool

local frames = 0
local got_image = false

function update()
    frames = frames + 1
    -- Fetch a picture once and cache it to the SD card. Runs off the main
    -- thread, so a slow download never stutters the menu.
    if not got_image then
        got_image = net_download(
            "https://http.cat/200.jpg",
            "sdmc:/slaunch/cache/widget_example.jpg")
    end
end

function render(x, y, w)
    local h = 150
    local ar, ag, ab = theme_color("accent")
    local dr, dg, db = theme_color("dim")

    -- Card background + accent strip, matching the active theme.
    gfx_fill_rect(x, y, w, h, 20, 20, 20, 150)
    gfx_fill_rect(x, y, w, 3, ar, ag, ab, 255)
    gfx_text_ex(0, x + 16, y + 12, "EXAMPLE WIDGET", dr, dg, db)

    -- Downloaded image on the left, once it is ready.
    if got_image then
        gfx_image("sdmc:/slaunch/cache/widget_example.jpg", x + 16, y + 40, 96, 96)
    end

    -- A little live text on the right.
    local msg = "frames: " .. frames
    gfx_text_ex(1, x + 128, y + 56, msg, ar, ag, ab)

    return h + 12   -- height consumed (+ a small gap)
end
