widget = {
  name = "Clock",
  options = {},
}

local DAYS   = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" }
local MONTHS = { "January", "February", "March", "April", "May", "June",
                 "July", "August", "September", "October", "November", "December" }

function update() end

-- Shrink a string until it fits, adding an ellipsis.
local function fit(s, size, max_w)
  if gfx_text_width_ex(size, s) <= max_w then return s end
  while #s > 5 and gfx_text_width_ex(size, s .. "...") > max_w do
    s = s:sub(1, -2)
  end
  return s .. "..."
end

-- box_h is the height the caller wants filled (0 = whatever the content needs,
-- which is what the floating home screen asks for). Everything below is placed
-- as a fraction of the height actually in use, so the frame fills the box and
-- the lines spread out instead of huddling at the top of a tall one.
function render(x, y, w, box_h)
  local ar, ag, ab = theme_color("accent")
  local fr, fg, fb = theme_color("fg")
  local dr, dg, db = theme_color("dim")

  local t          = os.date("*t")
  local time_str   = string.format("%02d:%02d", t.hour or 0, t.min or 0)
  local day_name   = DAYS[t.wday] or ""
  local month_name = MONTHS[t.month] or ""
  local date_str   = string.format("%s, %d %s", day_name, t.day or 1, month_name)

  local natural = 96
  -- Only ever grow into the box. Asked for less than the content needs, the
  -- natural layout is returned instead and the caller scales it down - text
  -- has a minimum readable size, and squeezing rows past it just overlaps
  -- them.
  local h = (box_h and box_h > natural) and box_h or natural
  local pad = math.max(10, math.floor(w * 0.05))
  local inner = w - pad * 2

  gfx_fill_rect(x, y, w, h, 20, 20, 20, 150)
  gfx_fill_rect(x, y, w, math.max(3, math.floor(h * 0.03)), ar, ag, ab, 255)

  -- Pick the largest size that still leaves room for the other two lines. On a
  -- short box that lands on the same layout the widget always had; on a tall
  -- one the time grows into the space instead of the space sitting empty.
  local size = 3
  while size > 1 and gfx_text_height(size) > h * 0.45 do size = size - 1 end
  local label_sz = (h > 160) and 1 or 0
  local date_sz  = (h > 220) and 1 or 0

  local lh_label = gfx_text_height(label_sz)
  local lh_time  = gfx_text_height(size)
  local lh_date  = gfx_text_height(date_sz)

  -- Three lines, with the leftover height split evenly between and around them.
  local gap = math.max(4, math.floor((h - lh_label - lh_time - lh_date) / 4))
  local cy  = math.floor((h - (lh_label + lh_time + lh_date + gap * 2)) / 2)

  local function centred(s, sz, ty)
    local str = fit(s, sz, inner)
    gfx_text_ex(sz, x + pad + math.floor((inner - gfx_text_width_ex(sz, str)) / 2), ty, str,
                fr, fg, fb)
  end

  local ty = y + cy
  local lab = fit("CLOCK", label_sz, inner)
  gfx_text_ex(label_sz, x + pad, ty, lab, dr, dg, db)
  ty = ty + lh_label + gap

  centred(time_str, size, ty)
  ty = ty + lh_time + gap

  local ds = fit(date_str, date_sz, inner)
  gfx_text_ex(date_sz, x + pad + math.floor((inner - gfx_text_width_ex(date_sz, ds)) / 2), ty,
              ds, dr, dg, db)

  -- Unconstrained (the floating home screen): keep the trailing gap the stacked
  -- layout has always had. In a box, return exactly what was asked for, so the
  -- caller can scale it in with nothing left over.
  if box_h and box_h > 0 then return h end
  return h + 12
end
