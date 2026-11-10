#include "social/DiscordService.hpp"

#include "discord/api.h"
#include "discord/parse.h"
#include "net/async.h"
#include "net/http.h"
#include "platform/platform.h"
#include "util/json.h"
#include "util/log.h"
#include "util/strings.h"

#include <switch.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <algorithm>
#include <functional>
#include <mutex>
#include <map>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {

// {"current": id, "accounts": [{id, username, global_name, avatar, token}],
//  "signed_out": [ids]} — the same account shape as SwitchCord's accounts.json.
constexpr const char* kAccountsPath = "sdmc:/slaunch/config/discord.json";

// What a paste brings along: spaces, line ends, the quotes around a copied header.
std::string cleanToken(std::string t) {
    const auto junk = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '"' || c == '\''; };
    while (!t.empty() && junk(t.back())) t.pop_back();
    size_t i = 0;
    while (i < t.size() && junk(t[i])) i++;
    return t.substr(i);
}

std::string readAll(const char* path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

// "Today at 18:04", "Yesterday at 09:30" or "12/10/2026", in the console's time zone.
std::string when(int64_t ms) {
    TimeCalendarTime at{}, now{};
    TimeCalendarAdditionalInfo info{};
    if (R_FAILED(timeToCalendarTimeWithMyRule(ms / 1000, &at, &info))) return {};
    u64 nowSec = 0;
    if (R_FAILED(timeGetCurrentTime(TimeType_UserSystemClock, &nowSec))
        || R_FAILED(timeToCalendarTimeWithMyRule(nowSec, &now, &info)))
        return {};
    char hm[8];
    std::snprintf(hm, sizeof(hm), "%02d:%02d", at.hour, at.minute);
    const bool sameMonth = at.year == now.year && at.month == now.month;
    if (sameMonth && at.day == now.day) return std::string("Today at ") + hm;
    if (sameMonth && at.day + 1 == now.day) return std::string("Yesterday at ") + hm;
    char d[16];
    std::snprintf(d, sizeof(d), "%02d/%02d/%04d", at.day, at.month, at.year);
    return d;
}

}  // namespace

// Stops a connection without waiting: update() drops it once its thread is done.
template <class T>
static void retire(std::unique_ptr<T>& p, std::vector<std::unique_ptr<T>>& bin) {
    if (!p) return;
    p->RequestStop();
    bin.push_back(std::move(p));
}

std::string DiscordService::Account::avatarUrl() const {
    User u;
    u.id = Str::ParseU64(id);
    u.avatar = avatar;
    return u.AvatarUrl(64);
}

DiscordService::DiscordService() = default;

DiscordService::~DiscordService() { shutdown(); }

void DiscordService::beginShutdown() {
    if (!m_started || m_stopping) return;
    m_stopping = true;
    Http::AbortAll();
    if (m_auth) m_auth->RequestStop();
    if (m_gateway) m_gateway->RequestStop();
    Async::RequestStop();
    Log::Close();
}

// Discord must never hold the menu up (closing for a game waited on it, and the
// old menu then never let go of HOME). Threads get a short time to stop; any
// still busy is left to finish on its own, with what it uses kept alive, and
// lingering() tells the menu to leave the sockets up for it.
void DiscordService::shutdown() {
    m_stopping = false;
    if (!m_started) return;
    Http::AbortAll(); // transfers in flight fail fast
    Log::Close();
    constexpr int kBudgetMs = 250;
    bool clean = true;
    if (m_auth && !m_auth->StopFor(kBudgetMs)) {
        (void)m_auth.release();
        clean = false;
    }
    if (m_gateway && !m_gateway->StopFor(kBudgetMs)) {
        (void)m_gateway.release();
        clean = false;
    }
    for (auto& g : m_oldGateways)
        if (!g->StopFor(kBudgetMs)) {
            (void)g.release();
            clean = false;
        }
    for (auto& a : m_oldAuths)
        if (!a->StopFor(kBudgetMs)) {
            (void)a.release();
            clean = false;
        }
    m_oldGateways.clear();
    m_oldAuths.clear();
    if (!Async::StopFor(kBudgetMs)) clean = false;
    if (!clean) (void)m_store.release(); // callbacks still running may touch it
    m_auth.reset();
    m_gateway.reset();
    m_lingering = m_lingering || !clean;
    m_started = false;
    LOG("[discord] stopped clean=%d", clean ? 1 : 0);
}

void DiscordService::loadAccounts() {
    m_accounts.clear();
    m_current.clear();
    m_signedOut.clear();
    auto add = [this](Account a) {
        a.token = cleanToken(a.token);
        if (a.token.empty()) return;
        if (a.token.empty()) return;
        if (!a.id.empty() && std::find(m_signedOut.begin(), m_signedOut.end(), a.id) != m_signedOut.end()) return;
        for (const Account& x : m_accounts)
            if (x.token == a.token || (!a.id.empty() && x.id == a.id)) return;
        m_accounts.push_back(std::move(a));
    };
    for (const char* path : {kAccountsPath}) {
        const std::string text = readAll(path);
        if (text.empty()) continue;
        J::Doc doc = J::Parse(text);
        yyjson_val* root = J::Root(doc);
        if (path == kAccountsPath) {
            m_current = J::S(root, "current");
            for (yyjson_val* id : J::Arr(root, "signed_out"))
                if (yyjson_is_str(id)) m_signedOut.push_back(yyjson_get_str(id));
            add({"", "", "", "", J::S(root, "token")}); // HomeNX's first format
        }
        for (yyjson_val* a : J::Arr(root, "accounts"))
            add({J::S(a, "id"), J::S(a, "username"), J::S(a, "global_name"), J::S(a, "avatar"), J::S(a, "token")});
    }
    ++m_accountsVersion;
}

void DiscordService::saveAccounts() const {
    std::string out = "{\"current\":" + Str::Json(m_current) + ",\"accounts\":[";
    for (size_t i = 0; i < m_accounts.size(); ++i) {
        const Account& a = m_accounts[i];
        out += (i ? ",\n  " : "\n  ");
        out += "{\"id\":" + Str::Json(a.id) + ",\"username\":" + Str::Json(a.username) +
               ",\"global_name\":" + Str::Json(a.globalName) + ",\"avatar\":" + Str::Json(a.avatar) +
               ",\"token\":" + Str::Json(a.token) + "}";
    }
    // Accounts signed out here stay out even though SwitchCord still lists them.
    out += "\n],\"signed_out\":[";
    bool first = true;
    for (const std::string& id : m_signedOut) {
        out += (first ? "" : ",") + Str::Json(id);
        first = false;
    }
    out += "]}\n";
    std::error_code ec;
    std::filesystem::create_directories("sdmc:/slaunch/config", ec);
    std::ofstream(kAccountsPath, std::ios::trunc) << out;
}

void DiscordService::activate() {
    if (m_stopping) shutdown(); // stopped for a launch that didn't happen: start again
    if (m_started) {
        // A token file added since this screen was first opened.
        if (m_state == State::SignedOut && m_store) {
            loadAccounts();
            LOG("opened signed out, %zu saved accounts", m_accounts.size());
            if (!m_accounts.empty()) { const Account pick = m_accounts.front(); connect(pick); }
        }
        return;
    }
    m_started = true;
    Log::Init("sdmc:/slaunch/discord.log");
    // The menu keeps the sockets and curl up; nifm is the only thing to open here.
    nifmInitialize(NifmServiceType_User);
    Http::ResetAbort();
    Async::Start();
    m_store = std::make_unique<Store>();
    loadAccounts();
    LOG("started, %zu saved accounts", m_accounts.size());
    if (m_accounts.empty()) {
        m_state = State::SignedOut;
        return;
    }
    const Account* pick = &m_accounts.front();
    for (const Account& a : m_accounts)
        if (a.id == m_current) pick = &a;
    connect(*pick);
}

void DiscordService::connect(const Account& account) {
    LOG("connecting an account, token %zu chars", account.token.size());
    retire(m_gateway, m_oldGateways);
    m_events.clear(); // the last account's
    m_store->Clear();
    m_token = account.token;
    m_current = account.id;
    m_rememberedMe = false;
    Api::SetToken(m_token);
    m_gateway = std::make_unique<Gateway>();
    if (!m_game.empty()) m_gateway->SetPresence(presenceJson());   // identify carries it
    m_gateway->Start(m_token);
    m_state = State::Connecting;
    m_error.clear();
    ++m_accountsVersion;
}

// Once Discord says who the token belongs to, keep the name and picture for the
// account switcher (and learn the id of a token saved before HomeNX kept ids).
void DiscordService::rememberMe() {
    m_rememberedMe = true;
    const User& me = m_store->me;
    const std::string id = Str::Id(me.id);
    m_accounts.erase(std::remove_if(m_accounts.begin(), m_accounts.end(),
                                    [&](const Account& a) { return a.id == id && a.token != m_token; }),
                     m_accounts.end());
    Account* mine = nullptr;
    for (Account& a : m_accounts)
        if (a.token == m_token) mine = &a;
    if (!mine) {
        m_accounts.push_back({});
        mine = &m_accounts.back();
    }
    *mine = {id, me.username, me.globalName, me.avatar, m_token};
    m_current = id;
    m_signedOut.erase(std::remove(m_signedOut.begin(), m_signedOut.end(), id), m_signedOut.end());
    saveAccounts();
    ++m_accountsVersion;
}

void DiscordService::update() {
    if (!m_started || m_stopping) return;
    Async::Pump();
    // Old connections (account switch, sign-in) close on their own threads.
    std::erase_if(m_oldGateways, [](const auto& g) { return g->Done(); });
    std::erase_if(m_oldAuths, [](const auto& a) { return a->Done(); });

    if (m_auth) {
        const RemoteAuth::Snapshot s = m_auth->Get();
        if (s.phase == RemoteAuth::Phase::Done && !s.token.empty()) {
            LOG("signed in by QR, connecting");
            retire(m_auth, m_oldAuths);
            Account added{s.userId, s.username, "", s.avatar, s.token};
            m_accounts.erase(std::remove_if(m_accounts.begin(), m_accounts.end(),
                                            [&](const Account& a) { return !a.id.empty() && a.id == added.id; }),
                             m_accounts.end());
            m_accounts.push_back(added);
            m_signedOut.erase(std::remove(m_signedOut.begin(), m_signedOut.end(), added.id), m_signedOut.end());
            connect(added);
            saveAccounts();
        } else if (s.phase == RemoteAuth::Phase::Failed) {
            if (m_error.empty()) LOG("QR sign-in failed: %s", s.error.c_str());
            m_error = s.error.empty() ? "The QR sign-in failed. Try again." : s.error;
        }
    }

    if (!m_gateway) return;
    // A burst (connecting: READY, member lists, presences) is applied over a
    // few frames, ~3 ms each, so HOME doesn't stutter while Discord connects.
    for (auto& e : m_gateway->Drain()) m_events.push_back(std::move(e));
    const uint64_t until = armGetSystemTick() + armNsToTicks(3'000'000ULL);
    while (!m_events.empty()) {
        Gateway::Event e = std::move(m_events.front());
        m_events.pop_front();
        const Store::Change ch = m_store->Apply(e.type, e.d);
        if (ch.kind == Store::Change::NewMessage && ch.mentionsMe) noteAlert(ch, e.d);
        if (armGetSystemTick() >= until) break;
    }
    State next = m_state;
    switch (m_gateway->GetState()) {
        case Gateway::State::Ready:
            if (m_store->ready) {
                next = State::Ready;
                if (!m_rememberedMe) rememberMe();
            }
            break;
        case Gateway::State::AuthFailed: {
            // The token no longer works (signed out elsewhere): forget it.
            retire(m_gateway, m_oldGateways);
            std::string who;
            for (const Account& a : m_accounts)
                if (a.token == m_token) who = a.name();
            m_accounts.erase(std::remove_if(m_accounts.begin(), m_accounts.end(),
                                            [&](const Account& a) { return a.token == m_token; }),
                             m_accounts.end());
            m_current.clear();
            saveAccounts();
            ++m_accountsVersion;
            next = State::SignedOut;
            m_error = (who.empty() ? std::string("This account") : who)
                      + " was signed out by Discord. Sign in again.";
            break;
        }
        case Gateway::State::Failed:
            next = State::Failed;
            m_error = m_gateway->LastError();
            break;
        default:
            if (m_state != State::Ready) next = State::Connecting;
            break;
    }
    if (!m_auth && m_state != State::TokenLogin) {  // keep the active sign-in screen stable
        if (m_state != next) LOG("state %d -> %d %s", (int)m_state, (int)next, m_error.c_str());
        m_state = next;
    }
}

std::string DiscordService::myName() const {
    return m_store ? m_store->me.DisplayName() : std::string();
}

std::string DiscordService::myUsername() const {
    return m_store ? m_store->me.username : std::string();
}

std::string DiscordService::myAvatarUrl() const {
    return m_store && m_store->me.id ? m_store->me.AvatarUrl(64) : std::string();
}

std::string DiscordService::myStatus() const {
    if (!m_store) return "offline";
    auto it = m_store->presence.find(m_store->me.id);
    return it != m_store->presence.end() && !it->second.empty() ? it->second : "online";
}

void DiscordService::switchAccount(const std::string& id) {
    for (const Account& a : m_accounts)
        if (a.id == id) {
            if (a.token == m_token && m_gateway) return;
            const Account pick = a;
            connect(pick);
            saveAccounts();
            return;
        }
}

void DiscordService::retry() {
    if (!m_store) { // the network never came up
        m_started = false;
        activate();
        return;
    }
    for (const Account& a : m_accounts)
        if (a.token == m_token) {
            const Account pick = a;
            connect(pick);
            return;
        }
    if (!m_accounts.empty()) {
        const Account pick = m_accounts.front();
        connect(pick);
        return;
    }
    startQrLogin();
}

void DiscordService::startQrLogin() {
    if (!m_started) activate();
    retire(m_auth, m_oldAuths);
    m_auth = std::make_unique<RemoteAuth>();
    m_auth->Start();
    m_state = State::QrLogin;
    m_error.clear();
}

void DiscordService::cancelQrLogin() {
    retire(m_auth, m_oldAuths);
    m_error.clear();
    // Back to whatever account was connected (update() works out its state).
    m_state = m_gateway ? (m_store && m_store->ready ? State::Ready : State::Connecting) : State::SignedOut;
}

void DiscordService::signInWithUserToken(const std::string& raw) {
    const std::string token = cleanToken(raw);
    LOG("token sign-in, %zu chars", token.size());
    if (token.empty()) {
        m_error = "Enter a Discord user session token.";
        m_state = State::SignedOut;
        return;
    }
    if (!m_started) activate();
    if (!m_started || (m_state == State::Failed && !m_store)) return;
    retire(m_auth, m_oldAuths);
    m_tokenLoginReturnState = m_state;
    m_state = State::TokenLogin;
    m_error.clear();
    ++m_accountsVersion;
    Api::FetchMe(token, [this, token](bool ok, User user, const std::string& err) {
        LOG("token check: ok=%d %s", ok ? 1 : 0, err.c_str());
        if (m_state != State::TokenLogin) return;
        if (!ok || !user.id) {
            m_state = m_tokenLoginReturnState;
            m_error = err.empty() ? "Discord didn't accept that user session token." : err;
            ++m_accountsVersion;
            return;
        }
        Account account{Str::Id(user.id), user.username, user.globalName, user.avatar, token};
        m_accounts.erase(std::remove_if(m_accounts.begin(), m_accounts.end(),
                                        [&](const Account& a) { return a.id == account.id; }), m_accounts.end());
        m_accounts.push_back(account);
        m_signedOut.erase(std::remove(m_signedOut.begin(), m_signedOut.end(), account.id), m_signedOut.end());
        connect(account);
        saveAccounts();
    });
}

std::string DiscordService::qrUrl() const {
    return m_auth ? m_auth->Get().qrUrl : std::string();
}

std::string DiscordService::qrStatus() const {
    if (!m_auth) return {};
    const RemoteAuth::Snapshot s = m_auth->Get();
    switch (s.phase) {
        case RemoteAuth::Phase::Preparing: return "Getting a code...";
        case RemoteAuth::Phase::ShowQr:    return "Scan this with the Discord mobile app: Settings > Scan QR Code.";
        case RemoteAuth::Phase::Confirm:   return "Confirm on your phone" + (s.username.empty() ? std::string() : " (" + s.username + ")") + ".";
        case RemoteAuth::Phase::LoggingIn: return "Signing in...";
        case RemoteAuth::Phase::Failed:    return s.error.empty() ? "The sign-in failed. Press A to try again." : s.error;
        default:                           return {};
    }
}

void DiscordService::signOut() {
    cancelQrLogin();
    retire(m_gateway, m_oldGateways);
    if (m_store) m_store->Clear();
    Api::SetToken("");
    const std::string token = m_token;
    m_token.clear();
    for (const Account& a : m_accounts)
        if (a.token == token && !a.id.empty()) m_signedOut.push_back(a.id);
    m_accounts.erase(std::remove_if(m_accounts.begin(), m_accounts.end(),
                                    [&](const Account& a) { return a.token == token; }),
                     m_accounts.end());
    m_current.clear();
    m_state = State::SignedOut;
    ++m_accountsVersion;
    if (!m_accounts.empty()) {
        const Account next = m_accounts.front();
        connect(next);
    }
    saveAccounts();
}

std::vector<DiscordService::Dm> DiscordService::dms(size_t max) const {
    std::vector<Dm> out;
    if (!m_store) return out;
    for (Snowflake id : m_store->dms) {
        const Channel* c = m_store->FindChannel(id);
        if (!c) continue;
        Dm d;
        d.channelId = id;
        d.name = m_store->ChannelName(*c);
        d.avatarUrl = m_store->DmAvatarUrl(*c);
        if (c->type == CH_DM && !c->recipients.empty()) {
            auto it = m_store->presence.find(c->recipients[0]);
            d.status = it != m_store->presence.end() && !it->second.empty() && it->second != "invisible"
                           ? it->second : "offline";
        } else {
            d.members = (int)c->recipients.size() + 1;
        }
        d.unread = m_store->ChannelUnread(id);
        d.mentions = m_store->ChannelMentions(id);
        out.push_back(std::move(d));
        if (out.size() >= max) break;
    }
    return out;
}

int DiscordService::dmMentions() const {
    return m_store ? m_store->DmMentions() : 0;
}

std::vector<DiscordService::Server> DiscordService::servers() const {
    std::vector<Server> out;
    if (!m_store) return out;
    std::vector<Snowflake> seen;
    auto add = [&](Snowflake id) {
        if (std::find(seen.begin(), seen.end(), id) != seen.end()) return;
        const Guild* g = m_store->FindGuild(id);
        if (!g || g->unavailable) return;
        seen.push_back(id);
        Server s;
        s.id = id;
        s.name = g->name;
        s.iconUrl = g->IconUrl(64);
        s.initials = g->Initials();
        s.unread = m_store->GuildUnread(id) && !m_store->GuildMuted(id);
        s.mentions = m_store->GuildMentions(id);
        out.push_back(std::move(s));
    };
    for (const GuildFolder& f : m_store->folders)
        for (Snowflake id : f.guildIds) add(id);
    std::vector<const Guild*> rest; // not in a folder yet: by name
    for (const auto& [id, g] : m_store->guilds)
        if (std::find(seen.begin(), seen.end(), id) == seen.end()) rest.push_back(&g);
    std::sort(rest.begin(), rest.end(), [](const Guild* a, const Guild* b) { return Str::Lower(a->name) < Str::Lower(b->name); });
    for (const Guild* g : rest) add(g->id);
    return out;
}

std::vector<DiscordService::ChannelRow> DiscordService::channels(Snowflake guildId) const {
    std::vector<ChannelRow> out;
    if (!m_store) return out;
    for (const Store::Row& r : m_store->ChannelList(guildId)) {
        const Channel* c = m_store->FindChannel(r.id);
        if (!c) continue;
        ChannelRow row;
        row.id = r.id;
        row.name = c->name;
        row.category = r.category;
        if (!r.category) {
            if (!c->HasMessages()) continue; // forums and directories
            row.voice = c->IsVoice();
            row.unread = m_store->ChannelUnread(r.id) && !m_store->ChannelMuted(*c);
            row.mentions = m_store->ChannelMentions(r.id);
        }
        out.push_back(std::move(row));
    }
    // A category with nothing left under it isn't shown.
    std::vector<ChannelRow> tidy;
    for (size_t i = 0; i < out.size(); ++i)
        if (!out[i].category || (i + 1 < out.size() && !out[i + 1].category)) tidy.push_back(std::move(out[i]));
    return tidy;
}

int DiscordService::friendsOnline() const {
    if (!m_store) return 0;
    int n = 0;
    for (const auto& [id, status] : m_store->presence)
        if (id != m_store->me.id && (status == "online" || status == "idle" || status == "dnd")) ++n;
    return n;
}

int DiscordService::unreadDms() const {
    if (!m_store) return 0;
    int n = 0;
    for (Snowflake id : m_store->dms)
        if (m_store->ChannelUnread(id) || m_store->ChannelMentions(id) > 0) ++n;
    return n;
}

void DiscordService::openChannel(Snowflake channelId) {
    if (!m_store || !channelId) return;
    const Channel* ch = m_store->FindChannel(channelId);
    const Snowflake guildId = ch ? ch->guildId : 0;
    // A server channel only sends live messages once it's subscribed to.
    if (guildId && m_gateway) m_gateway->Subscribe(guildId, channelId);
    ChannelMessages& cm = m_store->Messages(channelId);
    if (cm.loaded || cm.loading) return;
    cm.loading = true;
    Store* store = m_store.get();
    Api::FetchMessages(channelId, 0, 40, [this, store, channelId, guildId](bool ok, std::vector<Message> msgs, const std::string&) {
        if (store != m_store.get()) return; // signed into another account meanwhile
        ChannelMessages& c = store->Messages(channelId);
        c.loading = false;
        if (!ok) { c.error = true; return; }
        // Server nicknames and role colours for the authors on this page.
        if (guildId && m_gateway) {
            const std::vector<Snowflake> unknown = store->UnknownMembers(guildId, msgs);
            if (!unknown.empty()) m_gateway->RequestMembers(guildId, unknown);
        }
        store->AddHistory(channelId, std::move(msgs), false, 40);
        const auto& list = store->Messages(channelId).list;
        if (!list.empty()) {
            store->MarkRead(channelId, list.back().id);
            Api::Ack(channelId, list.back().id);
        }
    });
}

// Mentions, channels and custom emoji as Discord shows them: @name, #channel, :emoji:.
namespace {

constexpr std::uint32_t kEmojiMark = 0xF0000; // + index into Line::emojis

void mark(std::string& out, std::vector<std::string>* emojis, std::string url) {
    if (!emojis || emojis->size() >= 0xFFFD) return;
    Str::AppendCp(out, kEmojiMark + static_cast<std::uint32_t>(emojis->size()));
    emojis->push_back(std::move(url));
}

bool emojiStart(std::uint32_t c) {
    return (c >= 0x1F000 && c <= 0x1FAFF) || (c >= 0x2600 && c <= 0x27BF) || (c >= 0x2B05 && c <= 0x2B55)
        || c == 0x203C || c == 0x2049 || (c >= 0x2194 && c <= 0x21AA) || c == 0x231A || c == 0x231B
        || c == 0x2328 || c == 0x23CF || (c >= 0x23E9 && c <= 0x23FA) || c == 0x24C2 || c == 0x25AA
        || c == 0x25AB || c == 0x25B6 || c == 0x25C0 || (c >= 0x25FB && c <= 0x25FE) || c == 0x2934
        || c == 0x2935 || c == 0x3030 || c == 0x303D || c == 0x3297 || c == 0x3299;
}

// Unicode emoji (with skin tones, ZWJ families, flags, keycaps) become pictures:
// Twemoji, the set Discord itself draws, named by their codepoints.
std::string markEmoji(const std::string& text, std::vector<std::string>* emojis) {
    std::vector<std::uint32_t> cps;
    for (const char* p = text.data(), *end = p + text.size(); p < end;) cps.push_back(Str::NextCp(p, end));
    std::string out;
    for (size_t i = 0; i < cps.size();) {
        const std::uint32_t c = cps[i];
        const bool keycap = ((c >= '0' && c <= '9') || c == '#' || c == '*') && i + 1 < cps.size()
            && (cps[i + 1] == 0x20E3 || (cps[i + 1] == 0xFE0F && i + 2 < cps.size() && cps[i + 2] == 0x20E3));
        const bool marked = (c == 0xA9 || c == 0xAE || c == 0x2122) && i + 1 < cps.size() && cps[i + 1] == 0xFE0F;
        if (!emojiStart(c) && !keycap && !marked) {
            Str::AppendCp(out, c);
            ++i;
            continue;
        }
        std::vector<std::uint32_t> seq{c};
        size_t j = i + 1;
        if (c >= 0x1F1E6 && c <= 0x1F1FF && j < cps.size() && cps[j] >= 0x1F1E6 && cps[j] <= 0x1F1FF)
            seq.push_back(cps[j++]); // a flag: two regional letters
        for (;;) {
            if (j < cps.size() && (cps[j] == 0xFE0F || cps[j] == 0x20E3 || (cps[j] >= 0x1F3FB && cps[j] <= 0x1F3FF)
                                   || (cps[j] >= 0xE0020 && cps[j] <= 0xE007F))) {
                seq.push_back(cps[j++]);
            } else if (j + 1 < cps.size() && cps[j] == 0x200D) {
                seq.push_back(cps[j++]);
                seq.push_back(cps[j++]);
            } else {
                break;
            }
        }
        const bool zwj = std::find(seq.begin(), seq.end(), 0x200D) != seq.end();
        std::string name;
        for (std::uint32_t cp : seq) {
            if (cp == 0xFE0F && !zwj) continue; // Twemoji's file naming
            char hex[12];
            std::snprintf(hex, sizeof(hex), "%s%x", name.empty() ? "" : "-", cp);
            name += hex;
        }
        mark(out, emojis, "https://cdn.jsdelivr.net/gh/jdecked/twemoji@15.1.0/assets/72x72/" + name + ".png");
        i = j;
    }
    return out;
}

// Markdown Discord would style: the markers go, the words stay.
std::string stripMarkdown(const std::string& s) {
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        if (i == 0 || s[i - 1] == '\n') { // headings, small text and quotes at line starts
            for (const char* prefix : {"### ", "## ", "# ", "-# ", "> "})
                if (s.compare(i, std::strlen(prefix), prefix) == 0) {
                    i += std::strlen(prefix);
                    break;
                }
        }
        if (i + 1 < s.size() && ((s[i] == '*' && s[i + 1] == '*') || (s[i] == '_' && s[i + 1] == '_')
                                 || (s[i] == '~' && s[i + 1] == '~') || (s[i] == '|' && s[i + 1] == '|'))) {
            i += 2;
            continue;
        }
        if (s[i] == '`') {
            ++i;
            continue;
        }
        out += s[i++];
    }
    return out;
}

// Tenor and Giphy "GIFs" are MP4s; both also serve a real GIF.
std::string gifOf(const Embed& e) {
    const std::string& v = !e.videoUrl.empty() ? e.videoUrl : e.thumbOrig;
    // The small GIF each site keeps (~200 px, under 1 MB): the full one can be
    // 10 MB, past what the menu fetches, and the chat draws it small anyway.
    if (v.find("giphy.com") != std::string::npos) {
        const size_t slash = v.rfind('/');
        if (v.rfind(".mp4") != std::string::npos && slash != std::string::npos) return v.substr(0, slash + 1) + "200w.gif";
    }
    const size_t host = v.find("media.tenor.com/");
    if (host != std::string::npos) { // .../<id><5-letter format>/<name>.<ext>; AAAAM is the small GIF
        const size_t slash = v.find('/', host + 16);
        const size_t dot = v.rfind('.');
        if (slash != std::string::npos && slash >= host + 21 && dot != std::string::npos && dot > slash)
            return v.substr(0, slash - 5) + "AAAAM" + v.substr(slash, dot - slash) + ".gif";
    }
    return {};
}

// Discord's media proxy resizes (and turns WebP into PNG) for us.
std::string sized(const std::string& url, int w, int h, float maxW = 400.f, float maxH = 300.f) {
    if (url.empty()) return url;
    int tw = w, th = h;
    if (tw > 0 && th > 0) {
        const float s = std::min(1.f, std::min(maxW / tw, maxH / th));
        tw = std::max(1, static_cast<int>(tw * s));
        th = std::max(1, static_cast<int>(th * s));
    }
    std::string out = url + (url.find('?') == std::string::npos ? "?" : "&") + "format=png";
    if (tw > 0 && th > 0) out += "&width=" + std::to_string(tw) + "&height=" + std::to_string(th);
    return out;
}

// A Discord emoji or sticker link: how emoji and stickers from other servers
// are sent (without Nitro, or by "fake Nitro" plugins). Its id, and whether it moves.
bool discordPicture(const std::string& url, const char* kind, std::string& id, bool& animated) {
    for (const char* host : {"https://cdn.discordapp.com/", "https://media.discordapp.net/"}) {
        const std::string prefix = std::string(host) + kind + "/";
        if (url.compare(0, prefix.size(), prefix) != 0) continue;
        size_t q = prefix.size();
        while (q < url.size() && std::isdigit(static_cast<unsigned char>(url[q]))) ++q;
        if (q == prefix.size() || q - prefix.size() > 20) return false;
        id = url.substr(prefix.size(), q - prefix.size());
        animated = url.compare(q, 4, ".gif") == 0 || url.find("animated=true", q) != std::string::npos;
        return true;
    }
    return false;
}

bool isDiscordPicture(const std::string& url) {
    std::string id;
    bool animated = false;
    return discordPicture(url, "emojis", id, animated) || discordPicture(url, "stickers", id, animated);
}

// Emoji and sticker links ([name](link), <link> or the bare link) as the
// picture alone: an emoji mark in the text, a sticker under it. The link, and
// the id in it, never show.
std::string pictureLinks(const std::string& s, std::vector<std::string>* emojis,
                         std::vector<DiscordService::Line::Media>& stickers) {
    std::string out;
    auto take = [&](const std::string& url) {
        std::string id;
        bool animated = false;
        if (discordPicture(url, "emojis", id, animated)) {
            mark(out, emojis, "https://cdn.discordapp.com/emojis/" + id + (animated ? ".gif" : ".png") + "?size=48");
            return true;
        }
        if (!discordPicture(url, "stickers", id, animated)) return false;
        if (stickers.size() < 2) {
            DiscordService::Line::Media md;
            md.w = md.h = 160;
            md.animated = animated;
            md.fallback = "https://media.discordapp.net/stickers/" + id + ".png?size=160";
            md.url = animated ? "https://media.discordapp.net/stickers/" + id + ".gif?size=160" : md.fallback;
            stickers.push_back(std::move(md));
        }
        return true;
    };
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '[') {
            const size_t mid = s.find("](", i);
            const size_t close = mid == std::string::npos ? mid : s.find(')', mid + 2);
            if (close != std::string::npos && mid - i < 80 && take(s.substr(mid + 2, close - mid - 2))) {
                i = close + 1;
                continue;
            }
        }
        const bool angled = s.compare(i, 9, "<https://") == 0;
        if (angled || s.compare(i, 8, "https://") == 0) {
            size_t end = angled ? s.find('>', i) : s.find_first_of(" \n\t", i);
            if (end == std::string::npos) end = s.size();
            const size_t from = i + (angled ? 1 : 0);
            if (take(s.substr(from, end - from))) {
                i = angled ? end + 1 : end;
                continue;
            }
        }
        out += s[i++];
    }
    return out;
}

} // namespace

std::string DiscordService::plainText(const std::string& s, std::vector<std::string>* emojis) const {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const size_t close = s[i] == '<' ? s.find('>', i) : std::string::npos;
        if (close == std::string::npos || close - i > 80) {
            out += s[i++];
            continue;
        }
        const std::string tag = s.substr(i + 1, close - i - 1);
        std::string rep;
        if (tag.rfind("@&", 0) == 0) {
            rep = "@role";
        } else if (tag.rfind("@", 0) == 0) {
            const User* u = m_store->FindUser(Str::ParseU64(tag.substr(tag[1] == '!' ? 2 : 1)));
            rep = "@" + (u ? u->DisplayName() : std::string("unknown-user"));
        } else if (tag.rfind("#", 0) == 0) {
            rep = "#" + m_store->ChannelName(Str::ParseU64(tag.substr(1)));
        } else if (tag.rfind(":", 0) == 0 || tag.rfind("a:", 0) == 0) {
            const size_t a = tag.find(':'), b = tag.find(':', a + 1);
            if (b != std::string::npos && emojis) { // a server's own emoji: its picture
                const bool animated = tag[0] == 'a';
                const std::string id = tag.substr(b + 1);
                std::string pic;
                mark(pic, emojis, "https://cdn.discordapp.com/emojis/" + id + (animated ? ".gif" : ".png") + "?size=48");
                out += pic;
                i = close + 1;
                continue;
            }
            if (b != std::string::npos) rep = tag.substr(a, b - a + 1);
        }
        if (rep.empty()) {
            out += s[i++];
            continue;
        }
        out += rep;
        i = close + 1;
    }
    return out;
}

std::string DiscordService::emojiMarks(const std::string& s, std::vector<std::string>& urls) {
    return markEmoji(s, &urls);
}

std::vector<DiscordService::Line> DiscordService::lines(Snowflake channelId, size_t max) const {
    std::vector<Line> out;
    if (!m_store) return out;
    auto it = m_store->messages.find(channelId);
    if (it == m_store->messages.end()) return out;
    const auto& list = it->second.list;
    const size_t start = list.size() > max ? list.size() - max : 0;
    for (size_t i = start; i < list.size(); ++i) {
        const Message& m = list[i];
        Line l;
        std::vector<Line::Media> stickers;
        std::string text = m.IsSystem() ? m.systemText
                                        : stripMarkdown(plainText(pictureLinks(m.content, &l.emojis, stickers), &l.emojis));
        text = markEmoji(text, &l.emojis);
        if (text.size() > 1200) { // the panel shows a few lines of it at most
            size_t cut = 1200;
            while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
            text = text.substr(0, cut) + "...";
        }
        l.media = std::move(stickers);
        for (const Attachment& a : m.attachments) {
            if (a.IsImage() && !a.Spoiler() && l.media.size() < 2) {
                Line::Media md;
                md.w = a.width;
                md.h = a.height;
                md.animated = Str::EndsWith(Str::Lower(a.filename), ".gif") || a.contentType == "image/gif";
                md.fallback = sized(a.proxyUrl.empty() ? a.url : a.proxyUrl, a.width, a.height);
                if (!md.animated) md.full = sized(a.proxyUrl.empty() ? a.url : a.proxyUrl, a.width, a.height, 1920.f, 1080.f);
                md.url = md.animated ? a.url : md.fallback;
                l.media.push_back(std::move(md));
            } else {
                text += (text.empty() ? "" : " ") + std::string("[file] ") + a.filename;
            }
        }
        for (const Embed& e : m.embeds) {
            if (l.media.size() >= 2) break;
            if (isDiscordPicture(e.url) || isDiscordPicture(e.imageOrig) || isDiscordPicture(e.thumbOrig))
                continue; // already shown from its link
            Line::Media md;
            if (e.type == "gifv") {
                md.w = e.thumbW;
                md.h = e.thumbH;
                md.fallback = sized(!e.thumbUrl.empty() ? e.thumbUrl : e.imageUrl, md.w, md.h);
                const std::string gif = gifOf(e);
                md.animated = !gif.empty();
                md.url = md.animated ? gif : md.fallback;
                // A message that's only the GIF's link shows just the GIF, like Discord.
                const std::string only = Str::Trim(m.content);
                if (only.rfind("http", 0) == 0 && only.find(' ') == std::string::npos) text.clear();
            } else if (!e.imageUrl.empty() || (e.type == "image" && !e.thumbUrl.empty())) {
                md.w = e.imageUrl.empty() ? e.thumbW : e.imageW;
                md.h = e.imageUrl.empty() ? e.thumbH : e.imageH;
                const std::string& orig = e.imageOrig.empty() ? e.thumbOrig : e.imageOrig;
                md.fallback = sized(e.imageUrl.empty() ? e.thumbUrl : e.imageUrl, md.w, md.h);
                md.full = sized(e.imageUrl.empty() ? e.thumbUrl : e.imageUrl, md.w, md.h, 1920.f, 1080.f);
                md.animated = Str::EndsWith(Str::Lower(orig), ".gif");
                md.url = md.animated ? orig : md.fallback;
                if (md.animated) md.full.clear();   // the viewer plays the chat's copy
                if (e.type == "image" && Str::Trim(m.content) == e.url) text.clear();
            } else {
                if (text.empty() && !e.title.empty()) text = e.title;
                continue;
            }
            if (!md.url.empty()) l.media.push_back(std::move(md));
        }
        for (const StickerItem& st : m.stickers) {
            if (st.format == 3 || l.media.size() >= 2) { // Lottie stickers aren't pictures
                text += (text.empty() ? "" : " ") + std::string("[sticker] ") + st.name;
                continue;
            }
            Line::Media md;
            md.w = md.h = 160;
            md.animated = st.format == 4;
            md.fallback = "https://media.discordapp.net/stickers/" + Str::Id(st.id) + ".png?size=160";
            md.url = md.animated ? "https://media.discordapp.net/stickers/" + Str::Id(st.id) + ".gif?size=160"
                                 : md.fallback;
            l.media.push_back(std::move(md));
        }
        if (text.empty() && l.media.empty() && !m.embeds.empty()) text = "[embed]";
        if (m.replyId || m.replied || m.replyMissing) {
            l.reply = true;
            if (m.replied) {
                const Message& r = *m.replied;
                l.replyAuthor = m_store->AuthorName(r);
                l.replyText = Str::OneLine(stripMarkdown(plainText(r.content)));   // the menu draws its emoji
                if (l.replyText.size() > 160) {
                    size_t cut = 160;
                    while (cut > 0 && (static_cast<unsigned char>(l.replyText[cut]) & 0xC0) == 0x80) --cut;
                    l.replyText = l.replyText.substr(0, cut) + "...";
                }
                if (l.replyText.empty()) l.replyText = !r.attachments.empty() ? "[picture]" : !r.stickers.empty() ? "[sticker]" : "[message]";
            } else {
                l.replyText = "Original message was deleted";
            }
        }
        l.color = m_store->AuthorColor(m);
        l.authorId = m.author.id;
        l.author = m_store->AuthorName(m);
        l.avatarUrl = m.author.AvatarUrl(64);
        l.text = Str::OneLine(text);
        l.ms = m.TimeMs();
        l.time = when(l.ms);
        l.mine = m.author.id == m_store->me.id;
        l.pending = m.pending;
        l.failed = m.failed;
        out.push_back(std::move(l));
    }
    return out;
}

bool DiscordService::loading(Snowflake channelId) const {
    if (!m_store) return false;
    auto it = m_store->messages.find(channelId);
    return it != m_store->messages.end() && it->second.loading;
}

void DiscordService::send(Snowflake channelId, const std::string& raw) {
    const std::string text = Str::Trim(raw);
    if (!m_store || !channelId || text.empty()) return;
    Message m;
    m.id = Str::SnowflakeFromMs(Platform::NowMs()) | 0x3FFFFF;
    m.channelId = channelId;
    m.author = m_store->me;
    m.content = text;
    m.nonce = Api::NewNonce();
    const std::string nonce = m.nonce;
    m_store->AddLocalEcho(std::move(m));
    Store* store = m_store.get();
    Api::SendMessage(channelId, text, nonce, 0, false, [store, nonce, channelId](bool ok, Message sent, const std::string&) {
        if (ok) store->ConfirmSent(nonce, std::move(sent));
        else store->MarkFailed(channelId, nonce);
    });
}

// sLaunch's Discord application: its icon shows when the game has no picture.
static constexpr const char* kRpcApp = "1558878887117721620";

static std::string PresenceJson(std::string status, const std::string& game, int64_t start,
                                const std::string& asset) {
    if (status == "offline") status = "online";
    std::string act = "[]";
    if (!game.empty()) {
        act = std::string("[{\"application_id\":\"") + kRpcApp + "\",\"name\":" + Str::Json(game) +
              ",\"type\":0,\"timestamps\":{\"start\":" + std::to_string(start) + "}";
        if (!asset.empty())
            act += ",\"assets\":{\"large_image\":" + Str::Json(asset) + ",\"large_text\":" + Str::Json(game) + "}";
        act += "}]";
    }
    return "{\"status\":" + Str::Json(status) + ",\"since\":0,\"activities\":" + act + ",\"afk\":false}";
}

std::string DiscordService::presenceJson() const {
    std::lock_guard<std::mutex> lk(m_presMu);
    return PresenceJson(myStatus(), m_game, m_gameStart, m_gameAsset);
}

// The menu stops running once a game is in front, so the picture is looked up,
// proxied and sent from the worker. The gateway it holds outlives it: replaced
// ones are retired, and shutdown stops the workers before freeing any.
void DiscordService::setGame(const std::string& name, int64_t startMs, std::function<std::string()> icon) {
    if (name == m_game) return;
    unsigned seq;
    {
        std::lock_guard<std::mutex> lk(m_presMu);
        m_game = name;
        m_gameStart = startMs;
        m_gameAsset.clear();
        seq = ++m_gameSeq;
    }
    LOG("presence: %s", name.empty() ? "(none)" : name.c_str());
    if (!m_gateway) return;
    m_gateway->SetPresence(presenceJson());
    if (name.empty() || !icon) return;
    Gateway* gw = m_gateway.get();
    const std::string status = myStatus();
    Async::Run(Async::Pool::Api, [this, gw, seq, name, startMs, status, icon] {
        const std::string url = icon();
        if (url.empty()) { LOG("presence: no picture for %s", name.c_str()); return; }
        std::string asset;
        {
            std::lock_guard<std::mutex> lk(m_presMu);
            auto f = m_assetFor.find(url);
            if (f != m_assetFor.end()) asset = f->second;
        }
        if (asset.empty()) {
            const Http::Response r = Api::Call("POST", std::string("/applications/") + kRpcApp + "/external-assets",
                                               "{\"urls\":[" + Str::Json(url) + "]}");
            const size_t k = r.body.find("\"external_asset_path\"");
            const size_t q = k == std::string::npos ? k : r.body.find('"', r.body.find(':', k) + 1);
            const size_t e = q == std::string::npos ? q : r.body.find('"', q + 1);
            if (!r.Ok() || e == std::string::npos) {
                LOG("presence: picture refused (%ld)", r.status);
                return;
            }
            asset = "mp:" + r.body.substr(q + 1, e - q - 1);
        }
        std::lock_guard<std::mutex> lk(m_presMu);
        m_assetFor[url] = asset;
        if (seq != m_gameSeq) return;   // another game since
        m_gameAsset = asset;
        gw->SetPresence(PresenceJson(status, name, startMs, asset));
        LOG("presence: picture set");
    });
}

void DiscordService::pauseForGame() {
    if (!m_gateway || m_paused) return;
    m_paused = true;
    m_gateway->SetPaused(true);
}

bool DiscordService::resumeFromGame() {
    if (!m_paused) return false;
    m_paused = false;
    if (!m_gateway) return false;
    m_gateway->SetPaused(false);
    if (!m_gateway->TakeMissed()) return false;
    // Messages went by unseen: start a fresh session rather than show a stale store.
    for (const Account& a : m_accounts)
        if (a.token == m_token) {
            const Account pick = a;
            connect(pick);
            return true;
        }
    return false;
}

void DiscordService::loadOlder(Snowflake channelId) {
    if (!m_store || !channelId) return;
    ChannelMessages& cm = m_store->Messages(channelId);
    if (!cm.loaded || cm.loading || cm.loadingOlder || cm.reachedStart) return;
    Snowflake oldest = 0;
    for (const Message& m : cm.list)
        if (!m.pending && !m.failed) { oldest = m.id; break; }
    if (!oldest) return;
    cm.loadingOlder = true;
    Store* store = m_store.get();
    const Channel* ch = m_store->FindChannel(channelId);
    const Snowflake guildId = ch ? ch->guildId : 0;
    Api::FetchMessages(channelId, oldest, 50, [this, store, channelId, guildId](bool ok, std::vector<Message> msgs, const std::string&) {
        if (store != m_store.get()) return;
        ChannelMessages& c = store->Messages(channelId);
        if (!ok) { c.loadingOlder = false; return; }
        if (guildId && m_gateway) {
            const std::vector<Snowflake> unknown = store->UnknownMembers(guildId, msgs);
            if (!unknown.empty()) m_gateway->RequestMembers(guildId, unknown);
        }
        store->AddHistory(channelId, std::move(msgs), true, 50);
    });
}

bool DiscordService::loadingOlder(Snowflake channelId) const {
    if (!m_store) return false;
    auto it = m_store->messages.find(channelId);
    return it != m_store->messages.end() && it->second.loadingOlder;
}

bool DiscordService::reachedStart(Snowflake channelId) const {
    if (!m_store) return false;
    auto it = m_store->messages.find(channelId);
    return it != m_store->messages.end() && it->second.reachedStart;
}

void DiscordService::sendFile(Snowflake channelId, const std::string& path, const std::string& raw) {
    if (!m_store || !channelId || path.empty()) return;
    const std::string text = Str::Trim(raw);
    Message m;
    m.id = Str::SnowflakeFromMs(Platform::NowMs()) | 0x3FFFFF;
    m.channelId = channelId;
    m.author = m_store->me;
    m.content = text;
    m.nonce = Api::NewNonce();
    Attachment a;
    a.filename = path.substr(path.find_last_of('/') + 1);
    m.attachments.push_back(a);
    const std::string nonce = m.nonce;
    m_store->AddLocalEcho(std::move(m));
    Store* store = m_store.get();
    Api::UploadFile(channelId, path, text, nonce, [store, nonce, channelId](bool ok, Message sent, const std::string&) {
        if (ok) store->ConfirmSent(nonce, std::move(sent));
        else store->MarkFailed(channelId, nonce);
    });
}

void DiscordService::noteAlert(const Store::Change& ch, yyjson_val* d) {
    const Channel* c = m_store->FindChannel(ch.channelId);
    if (myStatus() == "dnd" || !c || m_store->ChannelMuted(*c)) {
        LOG("[discord] alert skipped (%s)", !c ? "unknown chat" : myStatus() == "dnd" ? "do not disturb" : "muted");
        return;
    }
    LOG("[discord] alert for HOME");
    const Message m = Parse::MessageObj(d);
    Alert a;
    a.channelId = ch.channelId;
    a.guildId = c->IsDM() ? 0 : c->guildId;
    a.title = m_store->AuthorName(m);
    a.chat = c->IsDM() ? m_store->ChannelName(*c) : c->name;
    if (!c->IsDM()) a.title += " in #" + c->name;
    else if (c->type == CH_GROUP_DM) a.title += " in " + a.chat;
    else a.chatAvatar = m_store->DmAvatarUrl(*c);
    a.avatarUrl = m.author.AvatarUrl(64);
    a.text = Str::Trim(stripMarkdown(plainText(m.content)));
    if (a.text.empty()) a.text = !m.stickers.empty() ? "Sent a sticker" : !m.attachments.empty() ? "Sent a picture" : "Sent a message";
    m_alert = std::move(a);
    m_hasAlert = true;
}

bool DiscordService::takeAlert(Alert& out) {
    if (!m_hasAlert) return false;
    m_hasAlert = false;
    out = std::move(m_alert);
    return true;
}

bool DiscordService::hasSavedAccount() const {
    std::error_code ec;
    return std::filesystem::exists(kAccountsPath, ec);
}

