

widget = {
  name = "Clock",
  options = {
    { id = "hour24",  label = "24-hour",      type = "bool", default = true  },
    { id = "seconds", label = "Show seconds", type = "bool", default = true  },
  },
}

local DAYS   = { "Sunday", "Monday", "Tuesday", "Wednesday",
                 "Thursday", "Friday", "Saturday" }
local MONTHS = { "January", "February", "March", "April", "May", "June",
                 "July", "August", "September", "October", "November", "December" }

function update()
  -- Nothing to fetch; the clock reads the time in render().
end

function render(x, y, w)
  local h = 150
  local ar, ag, ab = theme_color("accent")
  local dr, dg, db = theme_color("dim")
  local fr, fg, fb = theme_color("fg")

  gfx_fill_rect(x, y, w, h, 20, 20, 20, 105)
  gfx_fill_rect(x, y, w, 3, ar, ag, ab, 255)
  gfx_text_ex(0, x + 18, y + 14, "CLOCK", dr, dg, db)

  local t = os.date("*t")

  -- Time. 24-hour by default; 12-hour appends AM/PM.
  local hh, suffix = t.hour, ""
  if not config.hour24 then
    suffix = (t.hour < 12) and " AM" or " PM"
    hh = t.hour % 12
    if hh == 0 then hh = 12 end
  end

  local time_str
  if config.seconds then
    time_str = string.format("%02d:%02d:%02d", hh, t.min, t.sec)
  else
    time_str = string.format("%02d:%02d", hh, t.min)
  end
  time_str = time_str .. suffix

  gfx_text_ex(3, x + 18, y + 42, time_str, fr, fg, fb)

  -- Date line: "Saturday, 11 July 2026".
  local date_str = string.format("%s, %d %s %d",
    DAYS[t.wday] or "", t.day, MONTHS[t.month] or "", t.year)
  gfx_text_ex(1, x + 18, y + 104, date_str, ar, ag, ab)

  return h + 12
end
