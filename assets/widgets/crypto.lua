
widget = {
  name = "Crypto",
  options = {
    { id = "coins",    label = "Coins",    type = "string", default = "bitcoin,ethereum" },
    { id = "currency", label = "Currency", type = "string", default = "usd" },
  },
}

local SYMBOL = { usd = "$", eur = "\xE2\x82\xAC", gbp = "\xC2\xA3", jpy = "\xC2\xA5" }

local rows = {}          -- { { name = "Bitcoin", price = 63000.0 }, ... }
local status = "loading"
local last_key = nil
local tick = 0

local function trim(s)
  return (s:gsub("^%s*(.-)%s*$", "%1"))
end

-- Pretty-print a coin id: "bitcoin" -> "Bitcoin", "avalanche-2" -> "Avalanche".
local function pretty(id)
  local base = id:gsub("%-%d+$", "")
  return (base:gsub("(%a)([%w]*)", function(a, b) return a:upper() .. b end))
end

-- Pull the fiat number for a coin out of the flat CoinGecko JSON response:
--   {"bitcoin":{"usd":63000.5},"ethereum":{"usd":3100.2}}
local function price_of(json, id, cur)
  local _, _, obj = json:find('"' .. id .. '"%s*:%s*(%b{})')
  if not obj then return nil end
  local n = obj:match('"' .. cur .. '"%s*:%s*(-?%d+%.?%d*)')
  return n and tonumber(n) or nil
end

local function fetch()
  local coins = trim(config.coins or "")
  local cur   = trim((config.currency or "usd")):lower()
  if coins == "" then status = "no coins"; return end

  local url = "https://api.coingecko.com/api/v3/simple/price?ids="
              .. coins .. "&vs_currencies=" .. cur
  local resp = net_get(url)
  if not resp then status = "offline"; return end

  local new_rows = {}
  for id in coins:gmatch("[^,]+") do
    id = trim(id)
    local p = price_of(resp, id, cur)
    if p then new_rows[#new_rows + 1] = { name = pretty(id), price = p } end
  end

  if #new_rows == 0 then status = "no data"; return end
  rows = new_rows
  status = "live"
end

function update()
  tick = tick + 1
  local key = (config.coins or "") .. "|" .. (config.currency or "")
  -- Refetch when the settings change, or roughly every 2 min (~6 Hz).
  if key ~= last_key or (tick % 720) == 1 then
    last_key = key
    fetch()
  end
end

function render(x, y, w)
  local ar, ag, ab = theme_color("accent")
  local dr, dg, db = theme_color("dim")
  local fr, fg, fb = theme_color("fg")

  local n = #rows
  local h = 52 + math.max(n, 1) * 30

  gfx_fill_rect(x, y, w, h, 20, 20, 20, 105)
  gfx_fill_rect(x, y, w, 3, ar, ag, ab, 255)
  gfx_text_ex(0, x + 18, y + 14, "CRYPTO", dr, dg, db)

  local sw = gfx_text_width(status)
  gfx_text_ex(0, x + w - sw - 16, y + 14, status, ar, ag, ab)

  local sym = SYMBOL[(config.currency or "usd"):lower()] or ""
  if n == 0 then
    gfx_text_ex(1, x + 18, y + 48, "Loading...", dr, dg, db)
  else
    -- Prices sit in a fixed right-hand column (left-aligned there): gfx_text_width
    -- only measures the Small font, so we avoid right-aligning Normal-size text.
    local pcol = x + w - 150
    for i = 1, n do
      local ry = y + 42 + (i - 1) * 30
      gfx_text_ex(1, x + 18, ry, rows[i].name, fr, fg, fb)

      local pstr = string.format("%s%s", sym,
        (rows[i].price >= 100) and string.format("%.0f", rows[i].price)
                               or  string.format("%.2f", rows[i].price))
      gfx_text_ex(1, pcol, ry, pstr, ar, ag, ab)
    end
  end

  return h + 12
end
