
widget = {
  name = "Countdown",
  options = {
    { id = "label", label = "Event",           type = "string", default = "New Year" },
    { id = "date",  label = "Date (YYYY-MM-DD)", type = "string", default = "2027-01-01" },
  },
}

-- Whole days between two Unix timestamps (target - now), truncated toward zero.
local function day_diff(now, target)
  return math.floor((target - now) / 86400)
end

function update()
  -- Nothing to fetch; the math is done in render().
end

function render(x, y, w)
  local h = 158
  local ar, ag, ab = theme_color("accent")
  local dr, dg, db = theme_color("dim")
  local fr, fg, fb = theme_color("fg")

  gfx_fill_rect(x, y, w, h, 20, 20, 20, 105)
  gfx_fill_rect(x, y, w, 3, ar, ag, ab, 255)
  gfx_text_ex(0, x + 18, y + 14, "COUNTDOWN", dr, dg, db)

  local label = config.label or ""
  local ystr, mstr, dstr = (config.date or ""):match("^(%d+)%-(%d+)%-(%d+)$")

  if not ystr then
    gfx_text_ex(1, x + 18, y + 60, "Set a date (YYYY-MM-DD)", dr, dg, db)
    return h + 12
  end

  -- Compare at local midnight so a same-day event reads as "Today", not a
  -- fractional day.
  local now_t = os.date("*t")
  local now   = os.time({ year = now_t.year, month = now_t.month, day = now_t.day })
  local target = os.time({ year = tonumber(ystr), month = tonumber(mstr),
                           day = tonumber(dstr) })
  local days = day_diff(now, target)

  local big, sub
  if days > 0 then
    big = string.format("%d", days)
    sub = (days == 1) and "day to go" or "days to go"
  elseif days == 0 then
    big = "Today"
    sub = "is the day"
  else
    big = string.format("%d", -days)
    sub = (days == -1) and "day ago" or "days ago"
  end

  gfx_text_ex(1, x + 18, y + 42, label, ar, ag, ab)
  gfx_text_ex(3, x + 18, y + 66, big, fr, fg, fb)
  gfx_text_ex(1, x + 18, y + 118, sub, dr, dg, db)

  return h + 12
end
