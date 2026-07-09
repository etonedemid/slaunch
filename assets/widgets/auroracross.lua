-- AuroraChat widget. Ported from the old hardcoded C++ widget.
-- Configure it in  Theming > Widgets > AuroraChat  (Enabled, Username,
-- Password). To send a message, edit the "Send message" option - it is sent
-- once and then cleared.

-- Enable/disable is controlled by the home menu (Theming > Widgets), not here.
widget = {
  name = "AuroraChat",
  options = {
    { id = "user",    label = "Username",     type = "string", default = "" },
    { id = "pass",    label = "Password",     type = "string", default = "" },
    { id = "send",    label = "Send message", type = "string", default = "" },
  },
}

local HOST = "104.236.25.60"
local API  = "http://104.236.25.60:6767"
local PORT = 3033
local MAX_LINES = 6

local token = nil
local sock = nil
local status = "off"
local lines = {}
local pending_send = nil

local function add_line(user, msg)
  local line = user .. ": " .. msg
  -- Strip the server's "auroracross:  from <user>" prefix so only the message
  -- (e.g. "ERROR: Connection failed") is shown.
  local stripped = line:gsub("^auroracross:  from %s*", "")
  lines[#lines + 1] = stripped
  while #lines > MAX_LINES do table.remove(lines, 1) end
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
    if u and m then add_line(u, m) end
  end
end

function render(x, y, w)
  local ar, ag, ab = theme_color("accent")
  local dr, dg, db = theme_color("dim")
  local fr, fg, fb = theme_color("fg")

  local n = #lines
  local h = 56 + n * 26
  gfx_fill_rect(x, y, w, h, 20, 20, 20, 105)
  gfx_fill_rect(x, y, w, 3, ar, ag, ab, 255)
  gfx_text_ex(0, x + 18, y + 12, "AURORACHAT", dr, dg, db)
  local sw = gfx_text_width(status)
  gfx_text_ex(0, x + w - sw - 16, y + 12, status, ar, ag, ab)

  for i = 1, n do
    gfx_text_ex(0, x + 14, y + 44 + (i - 1) * 26, lines[i], fr, fg, fb)
  end
  return h + 12
end
