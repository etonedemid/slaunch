

widget = {
  name = "Calendar",
  options = {
    { id = "monday", label = "Week starts Monday", type = "bool", default = false },
  },
}

local MONTHS = { "January", "February", "March", "April", "May", "June",
                 "July", "August", "September", "October", "November", "December" }
local DOW_SUN = { "S", "M", "T", "W", "T", "F", "S" }
local DOW_MON = { "M", "T", "W", "T", "F", "S", "S" }

local function days_in_month(year, month)
  local d = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 }
  if month == 2 and (year % 4 == 0 and (year % 100 ~= 0 or year % 400 == 0)) then
    return 29
  end
  return d[month]
end

function update()
  -- Nothing to fetch.
end

function render(x, y, w)
  local ar, ag, ab = theme_color("accent")
  local dr, dg, db = theme_color("dim")
  local fr, fg, fb = theme_color("fg")

  local t     = os.date("*t")
  local first = os.date("*t", os.time({ year = t.year, month = t.month, day = 1 }))

  -- Column (0-6) the 1st of the month falls in, given the week-start choice.
  local start_col = first.wday - 1            -- 0 = Sunday
  if config.monday then start_col = (start_col + 6) % 7 end

  local ndays = days_in_month(t.year, t.month)
  local rows  = math.ceil((start_col + ndays) / 7)

  local pad, cell_h = 18, 30
  local grid_w = w - pad * 2
  local col_w  = math.floor(grid_w / 7)
  local top    = 78                            -- y of the first date row

  local h = top + rows * cell_h + 12

  gfx_fill_rect(x, y, w, h, 20, 20, 20, 105)
  gfx_fill_rect(x, y, w, 3, ar, ag, ab, 255)
  gfx_text_ex(0, x + pad, y + 14, "CALENDAR", dr, dg, db)

  -- Month + year header, e.g. "July 2026".
  gfx_text_ex(1, x + pad, y + 36,
    string.format("%s %d", MONTHS[t.month] or "", t.year), fr, fg, fb)

  -- Weekday header row.
  local dow = config.monday and DOW_MON or DOW_SUN
  for c = 1, 7 do
    local cx = x + pad + (c - 1) * col_w
    local lw = gfx_text_width(dow[c])
    gfx_text_ex(0, cx + (col_w - lw) / 2, y + 60, dow[c], dr, dg, db)
  end

  -- Day cells, wrapping every 7 columns.
  for day = 1, ndays do
    local idx = start_col + (day - 1)
    local col = idx % 7
    local row = math.floor(idx / 7)
    local cx  = x + pad + col * col_w
    local cy  = y + top + row * cell_h

    local label = tostring(day)
    local lw    = gfx_text_width(label)
    local tx    = cx + (col_w - lw) / 2

    if day == t.day then
      -- Highlight today with a filled accent chip.
      gfx_fill_rect(cx + 2, cy - 2, col_w - 4, cell_h - 4, ar, ag, ab, 255)
      gfx_text_ex(0, tx, cy + 4, label, 20, 20, 20)
    else
      gfx_text_ex(0, tx, cy + 4, label, fr, fg, fb)
    end
  end

  return h + 12
end
