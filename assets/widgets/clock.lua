widget = {
  name = "Clock",
  options = {},
}

local DAYS   = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" }
local MONTHS = { "January", "February", "March", "April", "May", "June",
                 "July", "August", "September", "October", "November", "December" }

function update() end

function render(x, y, w)
  local ar, ag, ab = theme_color("accent")
  local fr, fg, fb = theme_color("fg")
  local dr, dg, db = theme_color("dim")

  local t    = os.date("*t")
  local time_str = string.format("%02d:%02d", t.hour or 0, t.min or 0)
  local day_name = DAYS[t.wday] or ""
  local month_name = MONTHS[t.month] or ""

  -- Abbreviated date line that fits within typical widget widths.
  local date_str = string.format("%s, %d %s", day_name, t.day or 1, month_name)

  local h = 96

  gfx_fill_rect(x, y, w, h, 20, 20, 20, 150)
  gfx_fill_rect(x, y, w, 3, ar, ag, ab, 255)
  gfx_text_ex(0, x + 16, y + 10, "CLOCK", dr, dg, db)

  -- Time on its own line (large), date below it (smaller).
  local tw = gfx_text_width(time_str)
  gfx_text_ex(3, x + 16 + (w - 32 - tw) / 2, y + 34, time_str, fr, fg, fb)

  -- Date line: if it would overflow the widget width, truncate with ellipsis.
  local dw = gfx_text_width(date_str)
  local max_w = w - 32
  if dw > max_w then
    while #date_str > 5 and gfx_text_width(date_str .. "...") > max_w do
      date_str = date_str:sub(1, -2)
    end
    date_str = date_str .. "..."
  end

  local ddw = gfx_text_width(date_str)
  gfx_text_ex(0, x + 16 + (w - 32 - ddw) / 2, y + 70, date_str, dr, dg, db)

  return h + 12
end
