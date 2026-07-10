# sLaunch Widgets

Widgets are small Lua scripts that draw on the home screen (weather, chat,
clocks, whatever you want). Drop a `.lua` file in `sdmc:/slaunch/widgets/` and it
loads on the next launch.

Two ship by default: `weather.lua` and `auroracross.lua`.

## How it works

- A background thread calls your `update()` a few times a second. This is where
  you hit the network - it may block, that's fine, it's off the render thread.
- The main thread calls your `render(x, y, w)` every frame. **Draw only here**,
  never touch the network. Return the pixel height you used.
- Widgets are enabled/disabled and positioned from the menut:
  - **Theming > Widgets**
  - On the home screen, touch a widget and drag to move it; the position is
    saved to `config/widget_pos.txt`.

```lua
function update()
  -- runs on a worker thread
end

function render(x, y, w)
  -- runs on the main thread; draw only
  gfx_fill_rect(x, y, w, 100, 20, 20, 20, 150)
  return 112   -- height used
end
```

## Config variables

Declare a `widget` table so the menu can list and edit your settings. The
resolved values live in a global `config` table (`config.city`, ...). They're
saved to `sdmc:/slaunch/config/widgets/<script>.cfg`.

```lua
widget = {
  name = "Weather",                 -- shown in Theming > Widgets
  options = {
    { id = "city", label = "City", type = "string", default = "London" },
    { id = "big",  label = "Large", type = "bool",  default = false },
  },
}
```

- `type` is `"string"`, `"bool"`, or `"int"`.
- Edit a value in **Theming > Widgets > <widget>** (keyboard for strings,
  Left/Right for bools).
- Optional hook: `on_config(id)` is called right after the user changes an
  option, so you can react (re-login, refetch, ...).

Enable/disable is **not** a widget option — the home menu owns it.

## Engine API

### Drawing (render only)

| Function | Notes |
| --- | --- |
| `gfx_fill_rect(x, y, w, h, r, g, b, a)` | filled rectangle |
| `gfx_text(x, y, text)` | small white text |
| `gfx_text_ex(size, x, y, text, r, g, b)` | `size` 0=Small 1=Normal 2=Large 3=Title |
| `gfx_text_width(text) -> px` | width of small text |
| `gfx_image(path, x, y, w, h) -> bool` | draw a jpg/png from the SD |
| `theme_color(name) -> r, g, b` | `"bg" "bg_top" "fg" "dim" "accent" "title"` |
| `screen_width() / screen_height()` | 1280 / 720 |

### Networking (update only)

| Function | Notes |
| --- | --- |
| `net_get(url) -> string \| nil` | HTTP(S) GET |
| `net_post(url, body, content_type?, authorization?) -> string \| nil` | HTTP(S) POST |
| `net_download(url, path) -> bool` | save a URL straight to a file |
| `net_tcp_connect(host, port) -> sock \| nil` | non-blocking socket |
| `net_tcp_send(sock, data) -> bool` | |
| `net_tcp_recv(sock) -> string \| "" \| nil` | data / nothing waiting / closed |
| `net_tcp_close(sock)` | |

Networking is refused during `render()` so a slow request can't stall a frame.
Certificate verification is off (the Switch has no CA bundle).

Standard Lua libraries available: `base`, `string`, `math`, `table`, `os`.

## Minimal example

```lua
widget = {
  name = "Clock",
  options = { { id = "label", label = "Label", type = "string", default = "TIME" } },
}

function update() end

function render(x, y, w)
  local ar, ag, ab = theme_color("accent")
  local fr, fg, fb = theme_color("fg")
  local h = 96
  gfx_fill_rect(x, y, w, h, 20, 20, 20, 150)
  gfx_fill_rect(x, y, w, 3, ar, ag, ab, 255)
  gfx_text_ex(0, x + 16, y + 12, config.label, ar, ag, ab)
  gfx_text_ex(3, x + 16, y + 40, os.date("%H:%M"), fr, fg, fb)
  return h + 12
end
```

See `assets/widgets/weather.lua` and `assets/widgets/auroracross.lua` for fuller
examples (HTTP JSON, TCP chat, config-driven behaviour).
