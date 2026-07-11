-- AuroraChat widget. Ported from the old hardcoded C++ widget.
-- Set the Username/Password in  Theming > Widgets > AuroraChat. To send a
-- message, edit the "Send message" option - it's sent once, then cleared.
-- Enable/disable is done by the home menu (Theming > Widgets), not here.
--
-- Messages support:
--   * \n            -> line break (literal backslash-n, as typed)
--   * long lines    -> word-wrapped to the card width (long links hard-break)
--   * image links   -> a message containing a link embeds the picture under it:
--                      direct .gif/.png/.jpg links, and share pages (klipy,
--                      tenor, giphy, ...) whose HTML exposes an og:image. GIFs
--                      show their first frame - SDL_image can't animate.
--
-- The card has a FIXED height (Max height, px) set in Theming > Widgets. Only
-- the most recent messages that fit are shown; older ones scroll off the top.
widget = {
  name = "AuroraChat",
  options = {
    { id = "user",   label = "Username",       type = "string", default = "" },
    { id = "pass",   label = "Password",       type = "string", default = "" },
    { id = "send",   label = "Send message",   type = "string", default = "" },
    { id = "height", label = "Max height (px)", type = "int",    default = 360 },
  },
}

local HOST = "104.236.25.60"
local API  = "http://104.236.25.60:6767"
local PORT = 3033
local MAX_MSGS = 20        -- history kept in memory (only the recent fit on screen)
local LINE_H   = 24        -- vertical step per wrapped text line (Small font)
local IMG_W    = 260       -- displayed image box (aspect isn't known, so fixed)
local IMG_H    = 150
local MIN_H, MAX_H = 120, 700   -- clamp for the user's Max-height option

local token = nil
local sock = nil
local status = "off"
local msgs = {}            -- { { text = "...", img = path|nil, wrapped =, wrap_w = }, ... }
local pending_send = nil
local img_cache = {}       -- message url -> local path (or false = tried, no image)
local img_seq = 0          -- unique counter so a new url never reuses a cached texture

-- First http(s) link in a string (trailing punctuation trimmed), or nil.
local function find_url(s)
  for url in s:gmatch("https?://%S+") do
    return (url:gsub("[%.,!%?%)%]}>\"']+$", ""))
  end
  return nil
end

local function is_direct_image(low)
  return low:find("%.gif") or low:find("%.png") or low:find("%.jpe?g")
      or low:find("%.webp")
end

-- Pull an og:image / twitter:image URL out of a share page's HTML, if present.
local function find_meta_image(html)
  local props = { "og:image:secure_url", "og:image", "twitter:image", "twitter:image:src" }
  for _, p in ipairs(props) do
    local a = html:match('property=["\']' .. p .. '["\'][^>]-content=["\']([^"\']+)')
           or html:match('content=["\']([^"\']+)["\'][^>]-property=["\']' .. p .. '["\']')
           or html:match('name=["\']' .. p .. '["\'][^>]-content=["\']([^"\']+)')
           or html:match('content=["\']([^"\']+)["\'][^>]-name=["\']' .. p .. '["\']')
    if a and a ~= "" then return (a:gsub("&amp;", "&")) end
  end
  return nil
end

-- Resolve a message link to a direct image URL (network; update thread only).
local function resolve_image_url(url)
  if is_direct_image(url:lower()) then return url end
  local html = net_get(url)                 -- fetch the share page
  if not html then return nil end
  return find_meta_image(html)              -- og:image / twitter:image, or nil
end

local function add_message(user, msg)
  msg = msg:gsub("\\n", "\n")               -- typed "\n" becomes a real line break

  local text = user .. ": " .. msg
  -- Strip the server's "auroracross: from <user>" prefix so system notices show
  -- just the message (e.g. "ERROR: Connection failed").
  text = text:gsub("^auroracross: from %s*", "")

  -- Show the text right away; if the message has a link, its image is resolved
  -- and downloaded later by process_pending_image() so a slow fetch never delays
  -- the message or stalls the live chat.
  msgs[#msgs + 1] = { text = text, img = nil, img_url = find_url(msg) }
  while #msgs > MAX_MSGS do table.remove(msgs, 1) end
end

-- Resolve + download one queued message image (network; update thread only).
-- The message text is already on screen, so this just makes the picture pop in a
-- moment later instead of blocking the whole line behind a ~1 MB download.
local function process_pending_image()
  for _, m in ipairs(msgs) do
    if m.img_url and not m.img_tried then
      m.img_tried = true
      local url = m.img_url
      local cached = img_cache[url]
      if cached ~= nil then m.img = cached or nil; return end
      local media = resolve_image_url(url)
      if media then
        img_seq = img_seq + 1
        local path = "sdmc:/slaunch/cache/aurorachat_" .. img_seq .. ".img"
        if net_download(media, path) then
          img_cache[url] = path; m.img = path; return
        end
      end
      img_cache[url] = false
      return
    end
  end
end

local function disconnect()
  if sock then net_tcp_close(sock); sock = nil end
end

-- The engine calls this right after the user edits one of our options.
function on_config(id)
  if id == "send" then
    local m = config.send or ""
    if m ~= "" then pending_send = m end
    config.send = ""            -- one-shot: don't resend every tick
  elseif id == "user" or id == "pass" then
    token = nil                 -- re-login / reconnect with the new settings
    disconnect()
  end
end

function update()
  local user = config.user or ""
  local pass = config.pass or ""

  -- 1) Log in for a token.
  if not token then
    if user == "" or pass == "" then status = "set login"; return end
    status = "connecting"
    local resp = net_post(API .. "/api/login", user .. "|" .. pass .. "|", "text/plain", nil)
    if not resp then status = "offline"; return end
    if resp:find("ERR") then status = "login failed"; return end
    token = resp:match("^[^|]*")
    if not token or token == "" then status = "login failed"; return end
  end

  -- 2) Connect the live-message socket.
  if not sock then
    sock = net_tcp_connect(HOST, PORT)
    if not sock then status = "offline"; return end
    status = "live"
  end

  -- 3) Send a queued outgoing message.
  if pending_send then
    net_post(API .. "/api/chat", pending_send .. "|general|", "text/plain", token)
    pending_send = nil
  end

  -- 4) Drain one incoming packet: "username|message|room".
  local data = net_tcp_recv(sock)
  if data == nil then
    disconnect()                -- server closed the socket
  elseif data ~= "" then
    local u, m = data:match("^([^|]*)|([^|]*)|")
    if u and m then add_message(u, m) end
  end

  -- 5) Fetch one queued image (text is already shown; keeps the chat responsive).
  process_pending_image()
end

-- Greedy word-wrap into `out`. A single token wider than `maxw` (e.g. a long
-- link) is hard-broken by character so it never runs off the card.
local function wrap_words(text, maxw, out)
  local line = ""
  local function flush() if line ~= "" then out[#out + 1] = line; line = "" end end
  for word in text:gmatch("%S+") do
    local rest = word                       -- don't mutate the for-loop variable
    while gfx_text_width(rest) > maxw and #rest > 1 do
      local cut = #rest
      while cut > 1 and gfx_text_width(rest:sub(1, cut)) > maxw do cut = cut - 1 end
      flush()
      out[#out + 1] = rest:sub(1, cut)
      rest = rest:sub(cut + 1)
    end
    local trial = (line == "") and rest or (line .. " " .. rest)
    if line ~= "" and gfx_text_width(trial) > maxw then
      flush(); line = rest
    else
      line = trial
    end
  end
  flush()
end

-- Split on newlines (keeping blanks) then word-wrap each paragraph to `maxw`.
local function wrap_message(text, maxw)
  local lines = {}
  for para in (text .. "\n"):gmatch("([^\n]*)\n") do
    if para == "" then lines[#lines + 1] = ""    -- preserve blank lines
    else wrap_words(para, maxw, lines) end
  end
  return lines
end

-- Pixel height a message occupies (text lines + optional image + gap).
local function block_h(m)
  local hh = #m.wrapped * LINE_H
  if m.img then hh = hh + IMG_H + 6 end
  return hh + 6
end

function render(x, y, w)
  local ar, ag, ab = theme_color("accent")
  local dr, dg, db = theme_color("dim")
  local fr, fg, fb = theme_color("fg")

  local pad    = 16
  local textw  = w - pad * 2
  local imgw   = math.min(IMG_W, textw)
  local top    = 46
  local botpad = 10

  -- Fixed height chosen by the user (clamped).
  local H = math.floor(tonumber(config.height) or 360)
  if H < MIN_H then H = MIN_H elseif H > MAX_H then H = MAX_H end
  local availH = math.max(0, H - top - botpad)

  -- (Re)wrap each message, cached until the card width changes.
  for _, m in ipairs(msgs) do
    if m.wrapped == nil or m.wrap_w ~= textw then
      m.wrapped = wrap_message(m.text, textw)
      m.wrap_w  = textw
    end
  end

  -- Pick the tail of messages that fits, newest first (always keep the newest).
  local first, used = #msgs + 1, 0
  for i = #msgs, 1, -1 do
    local bh = block_h(msgs[i])
    if i ~= #msgs and used + bh > availH then break end
    used  = used + bh
    first = i
  end

  -- Card + header.
  gfx_fill_rect(x, y, w, H, 20, 20, 20, 105)
  gfx_fill_rect(x, y, w, 3, ar, ag, ab, 255)
  gfx_text_ex(1, x + pad, y + 12, "AuroraChat", dr, dg, db)
  local sw = gfx_text_width(status)
  gfx_text_ex(0, x + w - sw - pad, y + 16, status, ar, ag, ab)

  -- Bottom-align the visible messages within the content area.
  local cy = y + top + math.max(0, availH - used)
  for i = first, #msgs do
    local m = msgs[i]
    for l = 1, #m.wrapped do
      gfx_text_ex(0, x + pad, cy, m.wrapped[l], fr, fg, fb)
      cy = cy + LINE_H
    end
    if m.img then
      gfx_image(m.img, x + pad, cy, imgw, IMG_H)   -- first frame for GIFs
      cy = cy + IMG_H + 6
    end
    cy = cy + 6
  end

  return H
end
