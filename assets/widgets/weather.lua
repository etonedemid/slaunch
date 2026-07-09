-- Weather widget (open-meteo).

-- Enable/disable is controlled by the home menu (Theming > Widgets), not here.
widget = {
  name = "Weather",
  options = {
    { id = "city", label = "City", type = "string", default = "London" },
  },
}

local valid = false
local temp, wind, code = 0, 0, 0
local place = ""
local last_city = nil
local tick = 0

local function weather_text(c)
  if c == 0 then return "Clear"
  elseif c == 1 then return "Mainly clear"
  elseif c == 2 then return "Partly cloudy"
  elseif c == 3 then return "Overcast"
  elseif c == 45 or c == 48 then return "Fog"
  elseif c >= 51 and c <= 57 then return "Drizzle"
  elseif c >= 61 and c <= 67 then return "Rain"
  elseif c >= 71 and c <= 77 then return "Snow"
  elseif c >= 80 and c <= 82 then return "Showers"
  elseif c >= 85 and c <= 86 then return "Snow showers"
  elseif c >= 95 then return "Thunderstorm"
  else return "--" end
end

local function url_encode(s)
  return (s:gsub("[^%w%-_%.]", function(ch)
    return string.format("%%%02X", string.byte(ch))
  end))
end

local function json_number(s, key, from)
  local _, _, num = s:find('"' .. key .. '"%s*:%s*(-?%d+%.?%d*)', from or 1)
  return num and tonumber(num) or nil
end

local function json_string(s, key)
  return s:match('"' .. key .. '"%s*:%s*"([^"]*)"')
end

local function fetch()
  local city = config.city or ""
  if city == "" then valid = false; return end

  -- 1) Geocode the city name to coordinates.
  local geo = net_get("https://geocoding-api.open-meteo.com/v1/search?count=1&language=en&name=" .. url_encode(city))
  if not geo then valid = false; return end
  local lat = json_number(geo, "latitude")
  local lon = json_number(geo, "longitude")
  if not lat or not lon then valid = false; return end
  place = json_string(geo, "name") or city

  -- 2) Current conditions for those coordinates.
  local url = string.format(
    "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f&current=temperature_2m,weather_code,wind_speed_10m",
    lat, lon)
  local wx = net_get(url)
  if not wx then valid = false; return end
  local cur = wx:find('"current"') or 1
  local tp = json_number(wx, "temperature_2m", cur)
  if not tp then valid = false; return end
  temp = tp
  code = json_number(wx, "weather_code", cur) or 0
  wind = json_number(wx, "wind_speed_10m", cur) or 0
  valid = true
end

function update()
  tick = tick + 1
  -- Refetch when the city changes, or roughly every 15 min (~6 Hz).
  if config.city ~= last_city or (tick % 5400) == 1 then
    last_city = config.city
    fetch()
  end
end

function render(x, y, w)
  local h = 175
  local ar, ag, ab = theme_color("accent")
  local dr, dg, db = theme_color("dim")
  local fr, fg, fb = theme_color("fg")

  gfx_fill_rect(x, y, w, h, 20, 20, 20, 105)
  gfx_fill_rect(x, y, w, 3, ar, ag, ab, 255)
  gfx_text_ex(0, x + 18, y + 14, "WEATHER", dr, dg, db)

  if not valid then
    gfx_text_ex(1, x + 18, y + 60, "Loading...", dr, dg, db)
  else
    gfx_text_ex(3, x + 18, y + 40,  string.format("%.0f", temp) .. "\xC2\xB0C", fr, fg, fb)
    gfx_text_ex(1, x + 18, y + 96,  weather_text(code), ar, ag, ab)
    gfx_text_ex(0, x + 18, y + 128, place, dr, dg, db)
    gfx_text_ex(0, x + 18, y + 152, string.format("Wind %.0f km/h", wind), dr, dg, db)
  end
  return h + 12
end
