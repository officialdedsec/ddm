// DDM's built-in BitTorrent engine: one libtorrent session for every torrent. A torrent download is a DDM task like
// any other (the engine's queue starts, pauses and resumes it); while it runs it lives in the session, and when it
// stops its progress is saved ("resume data") and it leaves the session. Finished torrents may stay to share (seed)
// until a ratio or a time is reached; what was sharing when DDM closed shares again when it starts.
#include "torrent.hpp"
#include "engine.hpp"
#include "http.hpp"
#include "settings.hpp"
#include "../common/json.hpp"
#include "../common/util.hpp"
#include "../common/version.hpp"

#include <libtorrent/session.hpp>
#include <libtorrent/session_params.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/torrent_status.hpp>
#include <libtorrent/read_resume_data.hpp>
#include <libtorrent/write_resume_data.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/peer_info.hpp>
#include <libtorrent/announce_entry.hpp>
#include <libtorrent/ip_filter.hpp>
#include <libtorrent/create_torrent.hpp>
#include <libtorrent/bencode.hpp>
#include <libtorrent/fingerprint.hpp>
#include <libtorrent/extensions/ut_metadata.hpp>
#include <libtorrent/extensions/ut_pex.hpp>
#include <libtorrent/extensions/smart_ban.hpp>
#include <libtorrent/info_hash.hpp>

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <atomic>
#include <condition_variable>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

namespace ddm {

bool isMagnet(const std::string& url) { return startsWith(lower(trim(url)), "magnet:?"); }
bool isTorrentFile(const std::string& u) {
    std::string l = lower(trim(u));
    size_t q = l.find_first_of("?#");
    if (q != std::string::npos) l = l.substr(0, q);
    return endsWith(l, ".torrent");
}

namespace {

namespace lt = libtorrent;

std::string btDir() { std::string d = joinPath(dataDir(), "torrents"); makeDirs(d); return d; }
std::string resumePath(const std::string& id) { return joinPath(btDir(), id + ".resume"); }
std::string optionsPath(const std::string& id) { return joinPath(btDir(), id + ".json"); }
std::string sourcePath(const std::string& id) { return joinPath(btDir(), id + ".torrent"); }

// ------------------------------------------------------------------ the session
struct Entry {
    lt::torrent_handle h;
    std::string id;
    bool seeding = false;             // finished and sharing (no DDM task is running it)
    int64_t seedStart = 0;            // when sharing began (ms)
    int64_t uploadAtSeedStart = 0;
    std::string error;
    std::atomic<bool>* resumeSaved = nullptr;
};

std::mutex g_mu;                      // guards everything below
std::unique_ptr<lt::session> g_ses;
bool g_failed = false;                // couldn't start: the old engine is used
std::map<std::string, std::shared_ptr<Entry>> g_byId;
std::condition_variable g_cv;         // resume data saved / storage moved
std::set<std::string> g_resumeDone;   // ids whose latest resume data arrived
std::map<std::string, std::string> g_moved;   // id -> new folder ("" while moving, "!error" on failure)
std::string g_listen, g_portmap;      // for engineStatus()
int g_dhtNodes = 0;
std::atomic<bool> g_quit{false};
std::thread g_alerts;
std::string g_settingsKey;            // what the session was last set up with
bool g_pexOn = true;
int64_t g_watchCheck = 0;

std::shared_ptr<Entry> entryOf(const lt::torrent_handle& h) {
    for (auto& kv : g_byId) if (kv.second->h == h) return kv.second;
    return nullptr;
}

// ------------------------------------------------------------------ per-torrent options (side file)
bt::Options loadOptions(const std::string& id) {
    bt::Options o;
    std::string text;
    if (!readFile(optionsPath(id), text)) return o;
    bool ok = false;
    Json j = Json::parse(text, &ok);
    if (!ok) return o;
    for (auto& p : j["priorities"].list()) o.priorities.push_back(p.i32());
    o.sequential = j["sequential"].boolean();
    o.firstLast = j["firstLast"].boolean();
    o.superSeed = j["superSeed"].boolean();
    o.uploadKBps = j["uploadKBps"].i32();
    o.maxPeers = j["maxPeers"].i32();
    o.ratioPct = j.has("ratioPct") ? j["ratioPct"].i32() : -1;
    o.seedMinutes = j.has("seedMinutes") ? j["seedMinutes"].i32() : -1;
    o.keepSeeding = j.has("keepSeeding") ? j["keepSeeding"].i32() : -1;
    for (auto& t : j["trackers"].list()) o.trackers.push_back(t.str());
    for (auto& t : j["removedTrackers"].list()) o.removedTrackers.push_back(t.str());
    o.set = true;
    return o;
}

void saveOptions(const std::string& id, const bt::Options& o) {
    Json j = Json::object();
    Json pr = Json::array();
    for (int p : o.priorities) pr.push(p);
    j["priorities"] = pr;
    j["sequential"] = o.sequential; j["firstLast"] = o.firstLast; j["superSeed"] = o.superSeed;
    j["uploadKBps"] = o.uploadKBps; j["maxPeers"] = o.maxPeers;
    j["ratioPct"] = o.ratioPct; j["seedMinutes"] = o.seedMinutes; j["keepSeeding"] = o.keepSeeding;
    Json tr = Json::array(); for (auto& t : o.trackers) tr.push(t); j["trackers"] = tr;
    Json rt = Json::array(); for (auto& t : o.removedTrackers) rt.push(t); j["removedTrackers"] = rt;
    writeFileAtomic(optionsPath(id), j.dump());
}

// "1,3-5" (chosen in Download File Info / X-Ray; 1 = the first file) → priorities
std::vector<int> prioritiesFromSelection(const std::string& sel, int files) {
    std::vector<int> p;
    if (trim(sel).empty() || files <= 0) return p;
    p.assign(static_cast<size_t>(files), 0);
    for (auto part : split(sel, ',')) {
        part = trim(part);
        size_t dash = part.find('-');
        int a = atoi(part.substr(0, dash).c_str()), b = dash == std::string::npos ? a : atoi(part.substr(dash + 1).c_str());
        for (int i = std::max(1, a); i <= std::min(files, b); i++) p[static_cast<size_t>(i - 1)] = 4;
    }
    return p;
}

// ------------------------------------------------------------------ settings
std::vector<std::string> trackerLines(const std::string& text) {
    std::vector<std::string> out;
    for (auto l : split(replaceAll(text, "\r", ""), '\n')) { l = trim(l); if (!l.empty() && l.find("://") != std::string::npos) out.push_back(l); }
    return out;
}

int listenPort() {
    Settings& s = settings();
    if (s.btPort <= 0 || s.btPort > 65535) {
        LARGE_INTEGER c; QueryPerformanceCounter(&c);
        s.btPort = 20000 + static_cast<int>((c.QuadPart ^ GetCurrentProcessId()) % 40000);   // fixed from now on: routers and peers remember it
        saveSettings();
    }
    return s.btPort;
}

lt::settings_pack packFromSettings() {
    const Settings& s = settings();
    lt::settings_pack p;
    p.set_int(lt::settings_pack::alert_mask, static_cast<int>(static_cast<std::uint32_t>(
        lt::alert_category::error | lt::alert_category::status | lt::alert_category::storage | lt::alert_category::port_mapping | lt::alert_category::dht)));
    int port = listenPort();
    p.set_str(lt::settings_pack::listen_interfaces, "0.0.0.0:" + std::to_string(port) + ",[::]:" + std::to_string(port));
    p.set_bool(lt::settings_pack::enable_upnp, s.btUpnp);
    p.set_bool(lt::settings_pack::enable_natpmp, s.btUpnp);
    p.set_bool(lt::settings_pack::enable_dht, s.btDht);
    p.set_bool(lt::settings_pack::enable_lsd, s.btLsd);
    p.set_bool(lt::settings_pack::enable_incoming_utp, s.btUtp);
    p.set_bool(lt::settings_pack::enable_outgoing_utp, s.btUtp);
    int enc = s.btEncryption == "require" ? lt::settings_pack::pe_forced : s.btEncryption == "off" ? lt::settings_pack::pe_disabled : lt::settings_pack::pe_enabled;
    p.set_int(lt::settings_pack::out_enc_policy, enc);
    p.set_int(lt::settings_pack::in_enc_policy, enc);
    p.set_int(lt::settings_pack::allowed_enc_level, lt::settings_pack::pe_both);
    p.set_bool(lt::settings_pack::prefer_rc4, s.btEncryption == "require");
    p.set_bool(lt::settings_pack::anonymous_mode, s.btAnonymous);
    p.set_int(lt::settings_pack::connections_limit, std::max(20, s.btMaxConnections));
    p.set_int(lt::settings_pack::unchoke_slots_limit, s.uploadSlots > 0 ? s.uploadSlots : -1);
    p.set_int(lt::settings_pack::download_rate_limit, s.speedLimitKBps > 0 ? s.speedLimitKBps * 1024 : 0);
    p.set_int(lt::settings_pack::upload_rate_limit, s.uploadLimitKBps > 0 ? s.uploadLimitKBps * 1024 : 0);
    p.set_str(lt::settings_pack::user_agent, std::string("DDM/") + DDM_VERSION);
    {
        int a = 0, b = 0, c = 0;
        sscanf(DDM_VERSION, "%d.%d.%d", &a, &b, &c);
        p.set_str(lt::settings_pack::peer_fingerprint, lt::generate_fingerprint("DD", a, b, c, 0));   // "-DD4020-"
    }
    p.set_str(lt::settings_pack::handshake_client_version, std::string("DDM ") + DDM_VERSION);
    // DDM's own queue decides what runs: libtorrent never pauses or starts torrents by itself
    p.set_int(lt::settings_pack::active_downloads, -1);
    p.set_int(lt::settings_pack::active_seeds, -1);
    p.set_int(lt::settings_pack::active_limit, -1);
    p.set_bool(lt::settings_pack::announce_to_all_trackers, true);
    p.set_bool(lt::settings_pack::announce_to_all_tiers, true);
    p.set_int(lt::settings_pack::aio_threads, 4);
    // proxy (Options → Connection)
    std::string px = http::proxyUrl();
    int type = lt::settings_pack::none;
    std::string host, user, pass;
    int pport = 0;
    if (!px.empty()) {
        std::string l = lower(px);
        bool socks = startsWith(l, "socks5"), web = startsWith(l, "http://") || startsWith(l, "https://");
        std::string rest = px.substr(px.find("://") == std::string::npos ? 0 : px.find("://") + 3);
        size_t at = rest.rfind('@');
        if (at != std::string::npos) {
            std::string cred = rest.substr(0, at);
            rest = rest.substr(at + 1);
            size_t c = cred.find(':');
            user = urlDecode(cred.substr(0, c));
            if (c != std::string::npos) pass = urlDecode(cred.substr(c + 1));
        }
        size_t slash = rest.find('/'); if (slash != std::string::npos) rest = rest.substr(0, slash);
        size_t colon = rest.rfind(':');
        host = colon == std::string::npos ? rest : rest.substr(0, colon);
        pport = colon == std::string::npos ? (socks ? 1080 : 8080) : atoi(rest.substr(colon + 1).c_str());
        if (socks) type = user.empty() ? lt::settings_pack::socks5 : lt::settings_pack::socks5_pw;
        else if (web) type = user.empty() ? lt::settings_pack::http : lt::settings_pack::http_pw;
    }
    p.set_int(lt::settings_pack::proxy_type, type);
    if (type != lt::settings_pack::none) {
        p.set_str(lt::settings_pack::proxy_hostname, host);
        p.set_int(lt::settings_pack::proxy_port, pport);
        p.set_str(lt::settings_pack::proxy_username, user);
        p.set_str(lt::settings_pack::proxy_password, pass);
        p.set_bool(lt::settings_pack::proxy_hostnames, true);
        p.set_bool(lt::settings_pack::proxy_peer_connections, s.btProxyPeers);
        p.set_bool(lt::settings_pack::proxy_tracker_connections, true);
    }
    return p;
}

std::string settingsKey() {
    const Settings& s = settings();
    std::ostringstream k;
    k << s.btPort << s.btUpnp << s.btDht << s.btLsd << s.btUtp << s.btEncryption << s.btAnonymous << s.btMaxConnections << s.uploadSlots
      << '|' << s.speedLimitKBps << '|' << s.uploadLimitKBps << '|' << http::proxyUrl() << s.btProxyPeers << '|' << s.btIpFilter;
    return k.str();
}

// eMule ipfilter.dat ("1.2.3.0 - 1.2.3.255 , 000 , name") and PeerGuardian .p2p ("name:1.2.3.0-1.2.3.255")
int loadIpFilter(lt::session& ses, const std::string& path) {
    lt::ip_filter f;
    int rules = 0;
    if (!path.empty()) {
        std::ifstream in(W(path).c_str());
        std::string line;
        while (std::getline(in, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#' || line[0] == '/') continue;
            std::string range = line;
            int level = 0;
            if (line.find(',') != std::string::npos) {          // .dat
                auto parts = split(line, ',');
                range = parts[0];
                if (parts.size() > 1) level = atoi(trim(parts[1]).c_str());
                if (level >= 128) continue;                     // ≥ 128 means allowed in this format
            } else if (line.rfind(':') != std::string::npos) range = line.substr(line.rfind(':') + 1);   // .p2p
            size_t d = range.find('-');
            if (d == std::string::npos) continue;
            lt::error_code e1, e2;
            auto a = lt::make_address(trim(range.substr(0, d)), e1), b = lt::make_address(trim(range.substr(d + 1)), e2);
            if (e1 || e2 || a.is_v4() != b.is_v4()) continue;
            try { f.add_rule(a, b, lt::ip_filter::blocked); rules++; } catch (...) {}
        }
    }
    ses.set_ip_filter(f);
    return rules;
}

void applySettingsLocked() {
    if (!g_ses) return;
    std::string key = settingsKey();
    if (key == g_settingsKey) return;
    bool filterChanged = g_settingsKey.empty() || g_settingsKey.substr(g_settingsKey.rfind('|')) != key.substr(key.rfind('|'));
    g_settingsKey = key;
    g_ses->apply_settings(packFromSettings());
    if (filterChanged) {
        int n = loadIpFilter(*g_ses, settings().btIpFilter);
        if (!settings().btIpFilter.empty()) logLine("torrents: blocklist " + std::to_string(n) + " ranges");
    }
}

// ------------------------------------------------------------------ alerts
void writeResume(const std::string& id, const lt::add_torrent_params& atp) {
    std::vector<char> buf = lt::write_resume_data_buf(atp);
    writeFileAtomic(resumePath(id), std::string(buf.begin(), buf.end()));
}

void handleAlerts() {
    std::vector<lt::alert*> alerts;
    std::lock_guard<std::mutex> g(g_mu);
    if (!g_ses) return;
    g_ses->pop_alerts(&alerts);
    for (lt::alert* a : alerts) {
        if (auto* r = lt::alert_cast<lt::save_resume_data_alert>(a)) {
            auto e = entryOf(r->handle);
            if (e) { writeResume(e->id, r->params); g_resumeDone.insert(e->id); g_cv.notify_all(); }
        } else if (auto* f = lt::alert_cast<lt::save_resume_data_failed_alert>(a)) {
            auto e = entryOf(f->handle);
            if (e) { g_resumeDone.insert(e->id); g_cv.notify_all(); }
        } else if (auto* m = lt::alert_cast<lt::metadata_received_alert>(a)) {
            m->handle.save_resume_data(lt::torrent_handle::save_info_dict);   // the torrent's info is kept: the next start needs no peers for it
        } else if (auto* te = lt::alert_cast<lt::torrent_error_alert>(a)) {
            auto e = entryOf(te->handle);
            if (e) e->error = te->error.message() + (te->filename() && *te->filename() ? std::string(" (") + te->filename() + ")" : "");
        } else if (auto* fe = lt::alert_cast<lt::file_error_alert>(a)) {
            auto e = entryOf(fe->handle);
            if (e) e->error = fe->error.message() + " (" + fe->filename() + ")";
        } else if (auto* sm = lt::alert_cast<lt::storage_moved_alert>(a)) {
            auto e = entryOf(sm->handle);
            if (e) { g_moved[e->id] = sm->storage_path(); g_cv.notify_all(); }
        } else if (auto* smf = lt::alert_cast<lt::storage_moved_failed_alert>(a)) {
            auto e = entryOf(smf->handle);
            if (e) { g_moved[e->id] = "!" + smf->error.message(); g_cv.notify_all(); }
        } else if (auto* ls = lt::alert_cast<lt::listen_succeeded_alert>(a)) {
            if (ls->socket_type == lt::socket_type_t::tcp && ls->address.is_v4()) g_listen = "Port " + std::to_string(ls->port);
        } else if (auto* lf = lt::alert_cast<lt::listen_failed_alert>(a)) {
            if (lf->address.is_v4()) { g_listen = "Port " + std::to_string(lf->port) + " can't be used (" + lf->error.message() + ")"; logLine("torrents: " + g_listen); }
        } else if (auto* pm = lt::alert_cast<lt::portmap_alert>(a)) {
            g_portmap = "open on the router (" + std::string(pm->map_transport == lt::portmap_transport::upnp ? "UPnP" : "NAT-PMP") + ")";
        } else if (auto* pe = lt::alert_cast<lt::portmap_error_alert>(a)) {
            if (g_portmap.empty()) g_portmap = "not opened on the router";
            (void)pe;
        } else if (auto* ds = lt::alert_cast<lt::dht_stats_alert>(a)) {
            int n = 0;
            for (auto& b : ds->routing_table) n += b.num_nodes;
            g_dhtNodes = n;
        }
    }
}

void saveSessionState() {
    if (!g_ses) return;
    try {
        lt::session_params sp = g_ses->session_state(lt::session::save_dht_state);
        std::vector<char> buf = lt::write_session_params_buf(sp, lt::session::save_dht_state);
        writeFileAtomic(joinPath(btDir(), "session.dat"), std::string(buf.begin(), buf.end()));
    } catch (...) {}
}

void seedingRules();
void watchFolder();

void alertLoop() {
    int64_t lastHousekeeping = 0, lastResumeAll = nowMs();
    while (!g_quit) {
        lt::session* ses = nullptr;
        { std::lock_guard<std::mutex> g(g_mu); ses = g_ses.get(); }
        if (!ses) { Sleep(200); continue; }
        ses->wait_for_alert(lt::milliseconds(500));
        if (g_quit) break;
        handleAlerts();
        int64_t now = nowMs();
        if (now - lastHousekeeping > 5000) {
            lastHousekeeping = now;
            { std::lock_guard<std::mutex> g(g_mu); applySettingsLocked(); if (g_ses && settings().btDht) g_ses->post_dht_stats(); }
            seedingRules();
            watchFolder();
        }
        if (now - lastResumeAll > 60 * 1000) {   // progress saved every minute (a crash or power cut loses little)
            lastResumeAll = now;
            std::lock_guard<std::mutex> g(g_mu);
            for (auto& kv : g_byId) kv.second->h.save_resume_data(lt::torrent_handle::save_info_dict);
        }
    }
}

// the session, started the first time a torrent needs it
lt::session* session() {
    std::lock_guard<std::mutex> g(g_mu);
    if (g_ses) return g_ses.get();
    if (g_failed) return nullptr;
    try {
        lt::session_params sp;
        std::string state;
        if (readFile(joinPath(btDir(), "session.dat"), state) && !state.empty()) {
            try { sp = lt::read_session_params(lt::span<char const>(state.data(), static_cast<std::ptrdiff_t>(state.size())), lt::session::save_dht_state); } catch (...) { sp = lt::session_params(); }
        }
        sp.settings = packFromSettings();
        sp.extensions.clear();
        g_ses.reset(new lt::session(std::move(sp)));
        g_ses->add_extension(&lt::create_ut_metadata_plugin);
        g_ses->add_extension(&lt::create_smart_ban_plugin);
        g_pexOn = settings().btPex;
        if (g_pexOn) g_ses->add_extension(&lt::create_ut_pex_plugin);
        g_settingsKey = "";
        applySettingsLocked();
        g_quit = false;
        g_alerts = std::thread(alertLoop);
        logLine("torrents: engine started (libtorrent " + std::string(lt::version()) + "), port " + std::to_string(settings().btPort));
    } catch (std::exception& e) {
        logLine(std::string("torrents: the engine couldn't start: ") + e.what());
        g_ses.reset();
        g_failed = true;
    }
    return g_ses.get();
}

// ------------------------------------------------------------------ adding and removing
// The source: a magnet, a local .torrent, or the .torrent downloaded from the web (kept for later starts).
bool sourceFor(const Task& t, std::string& src, std::string& err) {
    if (isMagnet(t.url) || fileExists(t.url)) { src = t.url; return true; }
    std::string saved = sourcePath(t.id);
    if (!fileExists(saved)) {
        RequestInfo r;
        r.url = t.url; r.referer = t.pageUrl; r.cookies = t.cookies; r.userAgent = t.userAgent;
        std::string body;
        long st = http::get(r, body, nullptr, 32 * 1024 * 1024, 60);
        if (st != 200 || body.empty() || body[0] != 'd') { err = st == 0 ? "Couldn't reach the server for the .torrent file." : "The .torrent file couldn't be downloaded."; return false; }
        writeFileAtomic(saved, body);
    }
    src = saved;
    return true;
}

void applyOptions(lt::torrent_handle& h, const bt::Options& o, int downKBps, bool seeding) {
    const Settings& s = settings();
    if (o.sequential || s.sequentialDefault) h.set_flags(lt::torrent_flags::sequential_download); else h.unset_flags(lt::torrent_flags::sequential_download);
    if (o.superSeed && seeding) h.set_flags(lt::torrent_flags::super_seeding); else h.unset_flags(lt::torrent_flags::super_seeding);
    h.set_download_limit(downKBps > 0 ? downKBps * 1024 : 0);
    h.set_upload_limit(o.uploadKBps > 0 ? o.uploadKBps * 1024 : 0);
    h.set_max_connections(o.maxPeers > 0 ? o.maxPeers : std::max(10, s.torrentConnections));
    auto ti = h.torrent_file();
    if (ti && !o.priorities.empty()) {
        std::vector<lt::download_priority_t> pr;
        for (int i = 0; i < ti->num_files(); i++) pr.push_back(lt::download_priority_t(static_cast<std::uint8_t>(i < static_cast<int>(o.priorities.size()) ? std::max(0, std::min(7, o.priorities[static_cast<size_t>(i)])) : 4)));
        h.prioritize_files(pr);
    }
    if (ti && (o.firstLast || s.btFirstLast)) {   // the first and last pieces of each wanted file first: videos can be previewed sooner
        auto const& fs = ti->files();
        for (lt::file_index_t f(0); f < fs.end_file(); ++f) {
            if (fs.pad_file_at(f) || fs.file_size(f) == 0) continue;
            if (!o.priorities.empty() && static_cast<int>(f) < static_cast<int>(o.priorities.size()) && o.priorities[static_cast<size_t>(static_cast<int>(f))] == 0) continue;
            auto first = fs.map_file(f, 0, 1).piece, last = fs.map_file(f, std::max<std::int64_t>(0, fs.file_size(f) - 1), 1).piece;
            h.piece_priority(first, lt::top_priority);
            h.piece_priority(last, lt::top_priority);
        }
    }
    for (auto& url : o.trackers) h.add_tracker(lt::announce_entry(url));
    if (!o.removedTrackers.empty()) {
        auto list = h.trackers();
        std::vector<lt::announce_entry> keep;
        for (auto& a : list) if (std::find(o.removedTrackers.begin(), o.removedTrackers.end(), a.url) == o.removedTrackers.end()) keep.push_back(a);
        if (keep.size() != list.size()) h.replace_trackers(keep);
    }
}

// Puts the task's torrent into the session (from its saved progress when there is one). Called with g_mu NOT held.
std::shared_ptr<Entry> addToSession(const Task& t, bool seeding, std::string& err) {
    lt::session* ses = session();
    if (!ses) { err = "The torrent engine couldn't start."; return nullptr; }
    {
        std::lock_guard<std::mutex> g(g_mu);
        auto it = g_byId.find(t.id);
        if (it != g_byId.end()) { it->second->seeding = seeding; return it->second; }
    }
    lt::add_torrent_params atp;
    lt::error_code ec;
    std::string resume;
    bool fromResume = false;
    if (readFile(resumePath(t.id), resume) && !resume.empty()) {
        atp = lt::read_resume_data(lt::span<char const>(resume.data(), static_cast<std::ptrdiff_t>(resume.size())), ec);
        fromResume = !ec && (atp.ti || atp.info_hashes.has_v1() || atp.info_hashes.has_v2());
        if (!fromResume) { atp = lt::add_torrent_params(); ec.clear(); }
    }
    if (!fromResume) {
        std::string src;
        if (!sourceFor(t, src, err)) return nullptr;
        if (isMagnet(src)) {
            atp = lt::parse_magnet_uri(src, ec);
            if (ec) { err = "This magnet link isn't valid."; return nullptr; }
        } else {
            auto ti = std::make_shared<lt::torrent_info>(src, ec);
            if (ec) { err = "This torrent file is damaged or isn't a torrent (" + ec.message() + ")."; return nullptr; }
            atp.ti = ti;
        }
        for (auto& url : trackerLines(settings().btExtraTrackers)) atp.trackers.push_back(url);
    }
    atp.save_path = t.dir;
    makeDirs(t.dir);
    atp.flags &= ~(lt::torrent_flags::auto_managed | lt::torrent_flags::paused | lt::torrent_flags::upload_mode | lt::torrent_flags::stop_when_ready | lt::torrent_flags::seed_mode);
    atp.flags |= lt::torrent_flags::update_subscribe;
    atp.flags &= ~lt::torrent_flags::duplicate_is_error;
    if (seeding) atp.flags &= ~lt::torrent_flags::super_seeding;
    atp.storage_mode = settings().btPreallocate ? lt::storage_mode_allocate : lt::storage_mode_sparse;
    // the files chosen in Download File Info / X-Ray (first start only; afterwards the torrent's own options)
    bt::Options o = loadOptions(t.id);
    if (!o.set && !t.videoFormat.empty() && atp.ti) {
        o.priorities = prioritiesFromSelection(t.videoFormat, atp.ti->num_files());
        o.set = true;
        saveOptions(t.id, o);
    }
    if (!o.priorities.empty() && atp.ti) {
        atp.file_priorities.clear();
        for (int i = 0; i < atp.ti->num_files(); i++) atp.file_priorities.push_back(lt::download_priority_t(static_cast<std::uint8_t>(i < static_cast<int>(o.priorities.size()) ? o.priorities[static_cast<size_t>(i)] : 4)));
    }
    lt::torrent_handle h;
    try { h = ses->add_torrent(std::move(atp), ec); } catch (std::exception& e) { err = e.what(); return nullptr; }
    if (ec || !h.is_valid()) { err = "The torrent couldn't be started (" + ec.message() + ")."; return nullptr; }
    auto e = std::make_shared<Entry>();
    e->h = h;
    e->id = t.id;
    e->seeding = seeding;
    {
        std::lock_guard<std::mutex> g(g_mu);
        g_byId[t.id] = e;
    }
    // a magnet without the selection applied yet: as soon as the file list arrives
    if (!o.set && !t.videoFormat.empty()) {
        std::string id = t.id, sel = t.videoFormat;
        std::thread([h, id, sel]() mutable {
            for (int i = 0; i < 600 && !g_quit; i++) {
                Sleep(500);
                if (!h.is_valid()) return;
                auto ti = h.torrent_file();
                if (!ti) continue;
                bt::Options o = loadOptions(id);
                if (o.set) return;
                o.priorities = prioritiesFromSelection(sel, ti->num_files());
                o.set = true;
                saveOptions(id, o);
                std::vector<lt::download_priority_t> pr;
                for (int p : o.priorities) pr.push_back(lt::download_priority_t(static_cast<std::uint8_t>(p)));
                h.prioritize_files(pr);
                return;
            }
        }).detach();
    }
    return e;
}

// Saves the torrent's progress and takes it out of the session (the files stay).
void removeFromSession(const std::string& id) {
    std::shared_ptr<Entry> e;
    {
        std::lock_guard<std::mutex> g(g_mu);
        auto it = g_byId.find(id);
        if (it == g_byId.end() || !g_ses) return;
        e = it->second;
        g_resumeDone.erase(id);
    }
    if (e->h.is_valid()) {
        e->h.pause(lt::torrent_handle::graceful_pause);
        e->h.save_resume_data(lt::torrent_handle::save_info_dict | lt::torrent_handle::flush_disk_cache);
        std::unique_lock<std::mutex> l(g_mu);
        g_cv.wait_for(l, std::chrono::seconds(6), [&] { return g_resumeDone.count(id) > 0; });
    }
    std::lock_guard<std::mutex> g(g_mu);
    if (g_ses && e->h.is_valid()) g_ses->remove_torrent(e->h);
    g_byId.erase(id);
    g_resumeDone.erase(id);
}

std::string seedingListPath() { return joinPath(btDir(), "seeding.txt"); }
void saveSeedingList() {
    std::string text;
    {
        std::lock_guard<std::mutex> g(g_mu);
        for (auto& kv : g_byId) if (kv.second->seeding) text += kv.first + "\n";
    }
    writeFileAtomic(seedingListPath(), text);
}

bool wantsSeeding(const std::string& id) {
    bt::Options o = loadOptions(id);
    return o.keepSeeding == 1 || (o.keepSeeding == -1 && settings().seedAfterFinish);
}

// shared enough? (ratio or time, the torrent's own or Options')
void seedingRules() {
    std::vector<std::string> done;
    {
        std::lock_guard<std::mutex> g(g_mu);
        for (auto& kv : g_byId) {
            auto& e = *kv.second;
            if (!e.seeding || !e.h.is_valid()) continue;
            bt::Options o = loadOptions(e.id);
            int ratioPct = o.ratioPct >= 0 ? o.ratioPct : settings().btRatioLimitPct;
            int minutes = o.seedMinutes >= 0 ? o.seedMinutes : settings().btSeedMinutes;
            lt::torrent_status st = e.h.status();
            double ratio = st.total_wanted > 0 ? static_cast<double>(st.all_time_upload) / static_cast<double>(st.total_wanted) : 0;
            bool enough = (ratioPct > 0 && ratio * 100.0 >= ratioPct) || (minutes > 0 && (nowMs() - e.seedStart) / 60000 >= minutes);
            if (enough) done.push_back(e.id);
        }
    }
    if (done.empty()) return;
    std::thread([done] {   // not on the alert thread: saving the progress waits for its alert
        for (auto& id : done) { logLine("torrents: shared enough, stopping " + id); removeFromSession(id); }
        saveSeedingList();
        for (auto& id : done) Engine::get().edit(id, [](Task&) {});
    }).detach();
}

// Options → Torrents → watch folder: .torrent files put there are added by themselves (renamed .added after)
void watchFolder() {
    std::string dir = trim(settings().btWatchDir);
    if (dir.empty() || nowMs() - g_watchCheck < 10000) return;
    g_watchCheck = nowMs();
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(W(joinPath(dir, "*.torrent")).c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    std::vector<std::string> found;
    do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) found.push_back(joinPath(dir, U(fd.cFileName))); } while (FindNextFileW(f, &fd));
    FindClose(f);
    for (auto& path : found) {
        std::string keep = joinPath(btDir(), "w" + newId() + ".torrent");
        if (!CopyFileW(W(path).c_str(), W(keep).c_str(), FALSE)) continue;
        if (!MoveFileExW(W(path).c_str(), W(path + ".added").c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW(W(path).c_str());
        Task t;
        t.kind = "torrent";
        t.url = keep;
        t.fileName = sanitizeFileName(stem(baseName(path)));
        t.category = "torrent";
        Engine::get().add(t, true);
        logLine("torrents: added from the watch folder: " + baseName(path));
    }
}

std::string hexHash(const lt::info_hash_t& ih) {
    std::ostringstream o;
    if (ih.has_v1()) o << ih.v1; else o << ih.v2;
    return o.str();
}

std::string niceState(const lt::torrent_status& st) {
    switch (st.state) {
        case lt::torrent_status::checking_files: return "Checking files… " + std::to_string(static_cast<int>(st.progress * 100)) + "%";
        case lt::torrent_status::checking_resume_data: return "Checking files…";
        case lt::torrent_status::downloading_metadata: return "Getting torrent info…";
        default: break;
    }
    return "";
}

} // namespace

// ================================================================== a running torrent download
DownloadResult runTorrentDownload(DownloadContext& ctx) {
    Task t;
    { std::lock_guard<std::mutex> g(*ctx.mu); t = *ctx.task; }
    auto fail = [&](const std::string& e) { std::lock_guard<std::mutex> g(*ctx.mu); ctx.task->error = e; return DownloadResult::Failed; };
    if (!session()) return fail("The torrent engine couldn't start (another program may be using its network port). Restart DDM and try again.");
    { std::lock_guard<std::mutex> g(*ctx.mu); ctx.task->status = "Connecting to peers…"; }
    if (ctx.changed) ctx.changed();
    std::string err;
    auto e = addToSession(t, false, err);
    if (!e) return fail(err);
    {
        std::lock_guard<std::mutex> g(g_mu);
        e->seeding = false;
        e->error.clear();
    }
    e->h.resume();
    int lastLimit = -2;
    bt::Options o = loadOptions(t.id);
    int64_t lastOptions = 0;
    bool complete = false;
    std::string lastErr;
    while (true) {
        if (ctx.stop && ctx.stop->load()) break;
        Sleep(500);
        if (!e->h.is_valid()) { lastErr = "The torrent was removed."; break; }
        int limit;
        { std::lock_guard<std::mutex> g(*ctx.mu); limit = ctx.task->limitKBps; }
        if (limit != lastLimit || nowMs() - lastOptions > 3000) {   // options changed in the Torrent window, or the speed limiter
            if (nowMs() - lastOptions > 3000) { o = loadOptions(t.id); lastOptions = nowMs(); }
            applyOptions(e->h, o, limit, false);
            lastLimit = limit;
        }
        lt::torrent_status st = e->h.status(lt::torrent_handle::query_name);
        {
            std::lock_guard<std::mutex> g(g_mu);
            if (!e->error.empty()) lastErr = e->error;
        }
        if (st.errc) { lastErr = st.errc.message(); break; }
        std::lock_guard<std::mutex> g(*ctx.mu);
        Task& k = *ctx.task;
        if (st.has_metadata) {
            std::string name = sanitizeFileName(st.name);
            if (!name.empty()) k.fileName = name;
            k.total = st.total_wanted;
            k.done = st.total_wanted_done;
            k.resumable = true;
        }
        k.speed = static_cast<double>(st.download_payload_rate);
        k.eta = (k.speed > 1 && st.total_wanted > 0) ? static_cast<int64_t>((st.total_wanted - st.total_wanted_done) / k.speed) : -1;
        k.activeConnections = st.num_peers;
        std::string s = niceState(st);
        if (s.empty()) {
            if (st.is_finished || st.state == lt::torrent_status::seeding || st.state == lt::torrent_status::finished) { complete = st.has_metadata && st.total_wanted_done >= st.total_wanted; s = "Finishing…"; }
            else if (st.num_peers == 0) s = "Connecting to peers…";
            else s = "Receiving data · " + std::to_string(st.num_seeds) + (st.num_seeds == 1 ? " seed" : " seeds") + " · " + std::to_string(st.num_peers - st.num_seeds) + (st.num_peers - st.num_seeds == 1 ? " peer" : " peers");
        }
        k.status = s;
        if (ctx.changed) ctx.changed();
        if (complete) break;
    }
    {
        std::lock_guard<std::mutex> g(*ctx.mu);
        ctx.task->speed = 0; ctx.task->activeConnections = 0;
    }
    if (complete) {
        // Options → Torrents → move finished torrents to …
        std::string moveTo = trim(settings().btMoveTo);
        if (!moveTo.empty() && !iequals(moveTo, t.dir) && makeDirs(moveTo)) {
            { std::lock_guard<std::mutex> g(g_mu); g_moved.erase(t.id); }
            e->h.move_storage(moveTo, lt::move_flags_t::dont_replace);
            std::unique_lock<std::mutex> l(g_mu);
            g_cv.wait_for(l, std::chrono::seconds(600), [&] { return g_moved.count(t.id) > 0; });
            std::string to = g_moved.count(t.id) ? g_moved[t.id] : "";
            l.unlock();
            if (!to.empty() && to[0] != '!') { std::lock_guard<std::mutex> g(*ctx.mu); ctx.task->dir = to; }
            else logLine("torrents: couldn't move " + t.fileName + " to " + moveTo + (to.size() > 1 ? ": " + to.substr(1) : ""));
        }
        lt::torrent_status st = e->h.status();
        {
            std::lock_guard<std::mutex> g(*ctx.mu);
            ctx.task->total = st.total_wanted; ctx.task->done = st.total_wanted;
        }
        if (wantsSeeding(t.id)) {
            {
                std::lock_guard<std::mutex> g(g_mu);
                e->seeding = true;
                e->seedStart = nowMs();
            }
            applyOptions(e->h, loadOptions(t.id), 0, true);
            e->h.save_resume_data(lt::torrent_handle::save_info_dict);
            saveSeedingList();
        } else removeFromSession(t.id);
        return DownloadResult::Done;
    }
    removeFromSession(t.id);
    if (ctx.stop && ctx.stop->load()) return DownloadResult::Stopped;
    std::string l = lower(lastErr);
    std::lock_guard<std::mutex> g(*ctx.mu);
    if (l.find("space") != std::string::npos) ctx.task->error = "The disk is full.";
    else if (!lastErr.empty()) ctx.task->error = lastErr;
    else ctx.task->error = "The torrent stopped unexpectedly. Press Resume to continue.";
    return DownloadResult::Failed;
}

// ================================================================== seeding
std::set<std::string> g_seedPending;   // Share was clicked; it's being added to the session (shown as sharing at once)

bool torrentSeeding(const std::string& id) {
    std::lock_guard<std::mutex> g(g_mu);
    if (g_seedPending.count(id)) return true;
    auto it = g_byId.find(id);
    return it != g_byId.end() && it->second->seeding;
}

void startSeedingNow(const std::string& id) {
    Task t;
    if (!Engine::get().find(id, t) || t.kind != "torrent" || t.state != State::Complete) return;
    { std::lock_guard<std::mutex> g(g_mu); g_seedPending.insert(id); }
    std::thread([t] {
        std::string err;
        auto e = addToSession(t, true, err);
        if (!e) {
            { std::lock_guard<std::mutex> g(g_mu); g_seedPending.erase(t.id); }
            logLine("torrents: can't share " + t.fileName + ": " + err);
            Engine::get().edit(t.id, [](Task&) {});
            return;
        }
        { std::lock_guard<std::mutex> g(g_mu); e->seeding = true; e->seedStart = nowMs(); g_seedPending.erase(t.id); }
        e->h.resume();
        applyOptions(e->h, loadOptions(t.id), 0, true);
        saveSeedingList();
        Engine::get().edit(t.id, [](Task&) {});
    }).detach();
}

void stopSeeding(const std::string& id) {
    bool seeding;
    { std::lock_guard<std::mutex> g(g_mu); g_seedPending.erase(id); auto it = g_byId.find(id); seeding = it != g_byId.end() && it->second->seeding; }
    if (!seeding) return;
    removeFromSession(id);
    saveSeedingList();
}

void stopAllSeeding() {
    saveSeedingList();   // shared again on the next start
}

void restartSeeding() {
    std::string text;
    if (!readFile(seedingListPath(), text) || trim(text).empty()) return;
    std::thread([text] {
        Sleep(4000);   // after the list and the window are up
        for (auto id : split(text, '\n')) {
            id = trim(id);
            Task t;
            if (id.empty() || !Engine::get().find(id, t) || t.state != State::Complete || (!fileExists(t.path()) && !dirExists(t.path()))) continue;   // (a torrent of many files is a folder)
            startSeedingNow(id);
            Sleep(300);
        }
    }).detach();
}

void torrentShutdown() {
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> g(g_mu);
        if (!g_ses) return;
        for (auto& kv : g_byId) ids.push_back(kv.first);
    }
    for (auto& id : ids) {   // everything still in (sharing): progress saved
        std::shared_ptr<Entry> e;
        { std::lock_guard<std::mutex> g(g_mu); auto it = g_byId.find(id); if (it == g_byId.end()) continue; e = it->second; g_resumeDone.erase(id); }
        e->h.save_resume_data(lt::torrent_handle::save_info_dict | lt::torrent_handle::flush_disk_cache);
    }
    {
        std::unique_lock<std::mutex> l(g_mu);
        g_cv.wait_for(l, std::chrono::seconds(5), [&] { for (auto& id : ids) if (!g_resumeDone.count(id)) return false; return true; });
    }
    g_quit = true;
    if (g_alerts.joinable()) g_alerts.join();
    std::lock_guard<std::mutex> g(g_mu);
    saveSessionState();
    g_byId.clear();
    lt::session_proxy proxy = g_ses->abort();   // trackers are told we're leaving (briefly)
    g_ses.reset();
    (void)proxy;
}

void torrentSettingsChanged() {
    std::lock_guard<std::mutex> g(g_mu);
    if (!g_ses) return;
    applySettingsLocked();
    if (settings().btPex != g_pexOn) logLine("torrents: peer exchange changes when DDM starts again");
}

// ================================================================== the Torrent window
namespace bt {

Options options(const std::string& id) { return loadOptions(id); }

void setOptions(const std::string& id, const Options& o0) {
    Options o = o0;
    o.set = true;
    saveOptions(id, o);
    std::shared_ptr<Entry> e;
    { std::lock_guard<std::mutex> g(g_mu); auto it = g_byId.find(id); if (it != g_byId.end()) e = it->second; }
    if (!e || !e->h.is_valid()) return;
    Task t;
    int limit = Engine::get().find(id, t) ? t.limitKBps : 0;
    applyOptions(e->h, o, e->seeding ? 0 : limit, e->seeding);
    if (e->seeding && o.keepSeeding == 0) { std::thread([id] { stopSeeding(id); Engine::get().edit(id, [](Task&) {}); }).detach(); }
}

bool details(const std::string& id, Details& d, bool withPeers) {
    d = Details();
    std::shared_ptr<Entry> e;
    { std::lock_guard<std::mutex> g(g_mu); auto it = g_byId.find(id); if (it != g_byId.end()) e = it->second; }
    std::shared_ptr<const lt::torrent_info> ti;
    lt::add_torrent_params saved;
    bool haveSaved = false;
    Options o = loadOptions(id);
    if (e && e->h.is_valid()) {
        d.live = true;
        lt::torrent_status st = e->h.status(lt::torrent_handle::query_name | lt::torrent_handle::query_save_path | lt::torrent_handle::query_pieces | lt::torrent_handle::query_distributed_copies);
        ti = e->h.torrent_file();
        d.name = st.name; d.savePath = st.save_path;
        d.state = e->seeding ? "Sharing (seeding)" : niceState(st);
        if (d.state.empty()) d.state = st.is_finished ? "Finished" : st.num_peers ? "Downloading" : "Looking for peers";
        if (st.errc) d.error = st.errc.message();
        { std::lock_guard<std::mutex> g(g_mu); if (d.error.empty()) d.error = e->error; }
        d.wanted = st.total_wanted; d.done = st.total_wanted_done;
        d.uploaded = st.all_time_upload; d.downloaded = st.all_time_download;
        d.seeds = st.num_seeds; d.peers = st.num_peers - st.num_seeds;
        d.seedsInSwarm = st.num_complete; d.peersInSwarm = st.num_incomplete;
        d.availability = st.distributed_copies;
        d.down = st.download_payload_rate; d.up = st.upload_payload_rate;
        d.added = st.added_time; d.completed = st.completed_time;
        d.seedingSeconds = std::chrono::duration_cast<std::chrono::seconds>(st.seeding_duration).count();
        d.activeSeconds = std::chrono::duration_cast<std::chrono::seconds>(st.active_duration).count();
        d.piecesHave = st.num_pieces;
        const auto& bits = st.pieces;
        int n = bits.size();
        if (n > 0) { d.pieceMap.reserve(static_cast<size_t>(n)); for (int i = 0; i < n; i++) d.pieceMap += bits.get_bit(lt::piece_index_t(i)) ? '1' : '0'; }
        // files with their progress and priority
        if (ti) {
            std::vector<std::int64_t> prog;
            e->h.file_progress(prog, lt::torrent_handle::piece_granularity);
            auto pr = e->h.get_file_priorities();
            auto const& fs = ti->files();
            for (lt::file_index_t f(0); f < fs.end_file(); ++f) {
                if (fs.pad_file_at(f)) continue;
                FileRow r;
                r.path = fs.file_path(f);
                r.size = fs.file_size(f);
                int i = static_cast<int>(f);
                r.done = i < static_cast<int>(prog.size()) ? prog[static_cast<size_t>(i)] : 0;
                r.priority = i < static_cast<int>(pr.size()) ? static_cast<int>(static_cast<std::uint8_t>(pr[static_cast<size_t>(i)])) : 4;
                d.files.push_back(r);
            }
        }
        for (auto& a : e->h.trackers()) {
            TrackerRow r;
            r.url = a.url; r.tier = a.tier;
            bool updating = false, working = false;
            std::string msg;
            for (auto& ep : a.endpoints) for (auto& ih : ep.info_hashes) {
                if (ih.updating) updating = true;
                if (ih.fails == 0 && !ih.last_error && (ih.start_sent || ih.scrape_complete >= 0)) working = true;
                if (!ih.message.empty()) msg = ih.message;
                else if (ih.last_error) msg = ih.last_error.message();
                if (ih.scrape_complete >= 0) r.seeds = std::max(r.seeds, ih.scrape_complete);
                if (ih.scrape_incomplete >= 0) r.peers = std::max(r.peers, ih.scrape_incomplete);
            }
            r.working = working;
            r.status = updating ? "Updating…" : working ? (msg.empty() ? "Working" : "Working · " + msg) : msg.empty() ? "Not contacted yet" : msg;
            d.trackers.push_back(r);
        }
        if (withPeers) {
            std::vector<lt::peer_info> peers;
            e->h.get_peer_info(peers);
            for (auto& p : peers) {
                PeerRow r;
                std::ostringstream a;
                if (p.ip.address().is_v6()) a << '[' << p.ip.address().to_string() << "]:" << p.ip.port(); else a << p.ip.address().to_string() << ':' << p.ip.port();
                r.address = a.str();
                r.client = p.client;
                r.progress = p.progress;
                r.down = p.payload_down_speed; r.up = p.payload_up_speed;
                std::string f;
                if (p.flags & lt::peer_info::seed) f += "seed ";
                if (p.flags & (lt::peer_info::rc4_encrypted | lt::peer_info::plaintext_encrypted)) f += "encrypted ";
                if (p.flags & lt::peer_info::utp_socket) f += "µTP ";
                if (!(p.flags & lt::peer_info::local_connection)) f += "incoming ";
                if (p.source & lt::peer_info::dht) f += "DHT ";
                if (p.source & lt::peer_info::pex) f += "PEX ";
                if (p.source & lt::peer_info::lsd) f += "local ";
                r.flags = trim(f);
                d.peerList.push_back(r);
            }
        }
    } else {
        std::string resume;
        if (readFile(resumePath(id), resume) && !resume.empty()) {
            lt::error_code ec;
            saved = lt::read_resume_data(lt::span<char const>(resume.data(), static_cast<std::ptrdiff_t>(resume.size())), ec);
            haveSaved = !ec;
        }
        Task t;
        bool task = Engine::get().find(id, t);
        if (haveSaved) { ti = saved.ti; d.name = saved.name; d.savePath = saved.save_path; d.uploaded = saved.total_uploaded; d.downloaded = saved.total_downloaded; d.added = saved.added_time; d.completed = saved.completed_time; }
        std::string file = task && !isMagnet(t.url) ? (fileExists(t.url) ? t.url : sourcePath(id)) : "";
        if (!ti && !file.empty() && fileExists(file)) {
            lt::error_code ec;
            auto x = std::make_shared<lt::torrent_info>(file, ec);
            if (!ec) ti = x;
        }
        if (task) {
            if (d.name.empty()) d.name = t.fileName;
            if (d.savePath.empty()) d.savePath = t.dir;
            d.done = t.done; d.wanted = t.total > 0 ? t.total : 0;
            d.state = t.state == State::Complete ? "Finished" : t.state == State::Running ? "Starting…" : "Stopped";
        }
        if (ti) {
            auto const& fs = ti->files();
            for (lt::file_index_t f(0); f < fs.end_file(); ++f) {
                if (fs.pad_file_at(f)) continue;
                FileRow r;
                r.path = fs.file_path(f);
                r.size = fs.file_size(f);
                int i = static_cast<int>(f);
                r.priority = i < static_cast<int>(o.priorities.size()) ? o.priorities[static_cast<size_t>(i)] : 4;
                d.files.push_back(r);
            }
            if (haveSaved && saved.have_pieces.size() > 0) {
                for (int i = 0; i < saved.have_pieces.size(); i++) { bool b = saved.have_pieces.get_bit(lt::piece_index_t(i)); d.pieceMap += b ? '1' : '0'; if (b) d.piecesHave++; }
            }
        }
        if (haveSaved) for (auto& u : saved.trackers) { TrackerRow r; r.url = u; r.status = "—"; d.trackers.push_back(r); }
    }
    if (ti) {
        d.total = ti->total_size();
        d.pieces = ti->num_pieces();
        d.pieceSize = ti->piece_length();
        d.comment = ti->comment();
        d.creator = ti->creator();
        d.created = static_cast<int64_t>(ti->creation_date());
        d.isPrivate = ti->priv();
        d.v2 = ti->info_hashes().has_v2();
        d.hash = hexHash(ti->info_hashes());
    } else if (haveSaved) {
        d.hash = hexHash(saved.info_hashes);
    }
    if (d.wanted == 0) d.wanted = d.total;
    return !d.name.empty() || !d.files.empty();
}

void reannounce(const std::string& id) {
    std::lock_guard<std::mutex> g(g_mu);
    auto it = g_byId.find(id);
    if (it != g_byId.end() && it->second->h.is_valid()) {
        it->second->h.force_reannounce();
        if (settings().btDht) it->second->h.force_dht_announce();
    }
}

bool recheck(const std::string& id) {
    std::lock_guard<std::mutex> g(g_mu);
    auto it = g_byId.find(id);
    if (it == g_byId.end() || !it->second->h.is_valid()) return false;
    it->second->h.force_recheck();
    return true;
}

std::string engineStatus() {
    std::lock_guard<std::mutex> g(g_mu);
    if (!g_ses) return g_failed ? "The torrent engine couldn't start (see the log)." : "Not running (starts with the first torrent).";
    std::string s = g_listen.empty() ? "Starting…" : g_listen;
    if (!g_portmap.empty() && settings().btUpnp) s += " " + g_portmap;
    if (settings().btDht) s += " · DHT " + std::to_string(g_dhtNodes) + " nodes";
    int running = 0, sharing = 0;
    for (auto& kv : g_byId) (kv.second->seeding ? sharing : running)++;
    s += " · " + std::to_string(running) + " running, " + std::to_string(sharing) + " sharing";
    return s;
}

bool createTorrent(const CreateRequest& r, std::function<bool(int, int)> progress, std::string& magnet, std::string& error) {
    try {
        std::string src = r.source;
        while (src.size() > 3 && (src.back() == '\\' || src.back() == '/')) src.pop_back();
        if (!fileExists(src) && !dirExists(src)) { error = "The file or folder isn't there."; return false; }
        lt::file_storage fs;
        lt::add_files(fs, src, [](std::string const& p) {
            std::string n = lower(baseName(p));
            return !(n == "thumbs.db" || n == "desktop.ini" || n == ".ds_store" || endsWith(n, ".ddmpart"));
        });
        if (fs.num_files() == 0) { error = "There are no files in that folder."; return false; }
        lt::create_flags_t flags = r.version == "v2" ? lt::create_torrent::v2_only : r.version == "hybrid" ? lt::create_flags_t{} : lt::create_torrent::v1_only;
        lt::create_torrent ct(fs, r.pieceKB > 0 ? r.pieceKB * 1024 : 0, flags);
        int tier = 0;
        bool any = false;
        for (auto& line : r.trackers) {
            std::string u = trim(line);
            if (u.empty()) { if (any) tier++; any = false; continue; }
            ct.add_tracker(u, tier);
            any = true;
        }
        for (auto& w : r.webSeeds) if (!trim(w).empty()) ct.add_url_seed(trim(w));
        if (!r.comment.empty()) ct.set_comment(r.comment.c_str());
        ct.set_creator((std::string("DDM ") + DDM_VERSION).c_str());
        ct.set_priv(r.isPrivate);
        int total = ct.num_pieces();
        bool cancelled = false;
        lt::error_code ec;
        lt::set_piece_hashes(ct, dirName(src), [&](lt::piece_index_t i) {
            if (progress && !progress(static_cast<int>(i) + 1, total)) cancelled = true;
            if (cancelled) throw std::runtime_error("cancelled");
        }, ec);
        if (cancelled) { error = "Stopped."; return false; }
        if (ec) { error = ec.message(); return false; }
        std::vector<char> buf;
        lt::bencode(std::back_inserter(buf), ct.generate());
        if (!writeFileAtomic(r.out, std::string(buf.begin(), buf.end()))) { error = "The .torrent file couldn't be saved."; return false; }
        lt::error_code e2;
        lt::torrent_info ti(lt::span<char const>(buf.data(), static_cast<std::ptrdiff_t>(buf.size())), e2, lt::from_span);
        if (!e2) magnet = lt::make_magnet_uri(ti);
        return true;
    } catch (std::exception& e) {
        error = std::string(e.what()) == "cancelled" ? "Stopped." : e.what();
        return false;
    }
}

} // namespace bt

namespace {
void toRecycleBin(const std::string& p) {
    if (p.empty() || (!fileExists(p) && !dirExists(p))) return;
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    std::wstring from = W(p);
    from.push_back(L'\0');
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    int r = SHFileOperationW(&op);
    if (r != 0 || op.fAnyOperationsAborted) logLine("recycle " + p + ": failed (" + std::to_string(r) + ")");
    if (SUCCEEDED(co)) CoUninitialize();
}
} // namespace

std::string torrentMagnet(const std::string& id) {
    {   // in the engine now
        std::lock_guard<std::mutex> g(g_mu);
        auto it = g_byId.find(id);
        if (it != g_byId.end() && it->second->h.is_valid()) { std::string m = lt::make_magnet_uri(it->second->h); if (!m.empty()) return m; }
    }
    Task t;
    if (!Engine::get().find(id, t)) return "";
    if (isMagnet(t.url)) return trim(t.url);
    std::string resume;
    if (readFile(resumePath(id), resume) && !resume.empty()) {
        lt::error_code ec;
        lt::add_torrent_params atp = lt::read_resume_data(lt::span<char const>(resume.data(), static_cast<std::ptrdiff_t>(resume.size())), ec);
        if (!ec) { std::string m = lt::make_magnet_uri(atp); if (!m.empty()) return m; }
    }
    for (const std::string& f : {sourcePath(id), t.url}) {
        if (f.empty() || !fileExists(f)) continue;
        lt::error_code ec;
        lt::torrent_info ti(f, ec);
        if (!ec) return lt::make_magnet_uri(ti);
    }
    return "";
}

void torrentDeleteFiles(const Task& t, bool data, bool torrentFile) {
    if (data && !t.fileName.empty() && !t.dir.empty()) toRecycleBin(t.path());   // what arrived (a file or a folder), to the Recycle Bin
    if (torrentFile) {
        DeleteFileW(W(sourcePath(t.id)).c_str());
        DeleteFileW(W(resumePath(t.id)).c_str());
        DeleteFileW(W(optionsPath(t.id)).c_str());
        if (isTorrentFile(t.url) && fileExists(t.url)) toRecycleBin(t.url);   // the .torrent you opened
    }
}

void torrentDeleteData(const std::string& id) {
    std::thread([id] {
        Engine::get().pause(id);
        for (int i = 0; i < 300; i++) { Task t; if (!Engine::get().find(id, t) || t.state != State::Running) break; Sleep(50); }
        stopSeeding(id);
        removeFromSession(id);
        Task t;
        if (!Engine::get().find(id, t)) return;
        if (!t.fileName.empty() && !t.dir.empty()) toRecycleBin(t.path());
        DeleteFileW(W(resumePath(id)).c_str());   // its progress goes too: it starts again from 0% when you start it
        Engine::get().edit(id, [](Task& x) {
            x.state = State::Paused; x.pausedByUser = true; x.done = 0; x.segments.clear(); x.finished = 0; x.activeMs = 0; x.error.clear(); x.status = "Paused";
        });
    }).detach();
}

} // namespace ddm
