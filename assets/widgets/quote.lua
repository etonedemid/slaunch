

widget = {
  name = "Quote",
}

local quote  = ""
local author = ""
local status = "loading"
local tick   = 0

local function json_string(s, key)
  return s:match('"' .. key .. '"%s*:%s*"([^"]*)"')
end

-- Undo the couple of JSON escapes that show up in quote text.
local function unescape(s)
  s = s:gsub('\\"', '"')
  s = s:gsub("\\n", " ")
  s = s:gsub("\\/", "/")
  s = s:gsub("\\\\", "\\")
  return s
end

local function fetch()
  -- ZenQuotes returns: [{"q":"...the quote...","a":"Author","h":"..."}]
  local resp = net_get("https://zenquotes.io/api/random")
  if not resp then status = "offline"; return end
  local q = json_string(resp, "q")
  local a = json_string(resp, "a")
  if not q or q == "" then status = "no data"; return end
  quote  = unescape(q)
  author = unescape(a or "")
  status = "ok"
end

function update()
  tick = tick + 1
  -- Fetch once at startup, then about every 30 min (~6 Hz).
  if tick == 1 or (tick % 10800) == 1 then fetch() end
end

-- Greedy word-wrap `text` to `maxw` pixels (measured at the Small font).
local function wrap(text, maxw)
  local out, line = {}, ""
  for word in text:gmatch("%S+") do
    local trial = (line == "") and word or (line .. " " .. word)
    if gfx_text_width(trial) > maxw and line ~= "" then
      out[#out + 1] = line
      line = word
    else
      line = trial
    end
  end
  if line ~= "" then out[#out + 1] = line end
  return out
end

function render(x, y, w)
  local ar, ag, ab = theme_color("accent")
  local dr, dg, db = theme_color("dim")
  local fr, fg, fb = theme_color("fg")

  local pad = 18
  local lines = (quote ~= "") and wrap('"' .. quote .. '"', w - pad * 2) or {}
  local n = #lines

  -- Header + quote lines (+ 24 each) + author line + padding.
  local h = 44 + (n > 0 and (n * 24 + 30) or 30)

  gfx_fill_rect(x, y, w, h, 20, 20, 20, 105)
  gfx_fill_rect(x, y, w, 3, ar, ag, ab, 255)
  gfx_text_ex(0, x + pad, y + 14, "QUOTE", dr, dg, db)

  if n == 0 then
    gfx_text_ex(1, x + pad, y + 44, "Loading...", dr, dg, db)
    return h + 12
  end

  -- Drawn at the Small size to match wrap()'s gfx_text_width measurement.
  for i = 1, n do
    gfx_text_ex(0, x + pad, y + 40 + (i - 1) * 24, lines[i], fr, fg, fb)
  end

  if author ~= "" then
    local by = "- " .. author
    local bw = gfx_text_width(by)
    gfx_text_ex(0, x + w - bw - pad, y + 44 + n * 24, by, ar, ag, ab)
  end

  return h + 12
end
