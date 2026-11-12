#include "remote.hpp"
#include "crypto.hpp"
#include "engine.hpp"
#include "settings.hpp"
#include "webserver.hpp"
#include "../common/json.hpp"
#include "../common/util.hpp"
#include "../common/version.hpp"
#include <algorithm>
#include <map>
#include <mutex>

namespace ddm {
namespace remote {

namespace {
std::mutex g_mu;
web::Server g_server;
std::function<void(const std::string&, const std::string&, const std::string&)> g_handler;
std::map<std::string, std::pair<int, int64_t>> g_fails;   // ip -> (wrong keys, since)

const char* kPage = R"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="referrer" content="no-referrer"><title>DDM remote</title><style>
:root{--bg:#f4f6fa;--card:#fff;--fg:#141a24;--mut:#667085;--line:#e3e7ef;--acc:#0b84c6;--good:#16a34a;--warn:#d97706;--bad:#dc2626;color-scheme:light}
@media (prefers-color-scheme:dark){:root{--bg:#0f141c;--card:#18202b;--fg:#e8edf4;--mut:#93a0b2;--line:#273243;--acc:#38b2f0;--good:#34d27a;--warn:#f5a524;--bad:#f26d6d;color-scheme:dark}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.4 -apple-system,Segoe UI,Roboto,Arial,sans-serif;padding:env(safe-area-inset-top) max(14px,env(safe-area-inset-right)) 40px max(14px,env(safe-area-inset-left))}
header{display:flex;align-items:center;justify-content:space-between;padding:16px 2px 10px}h1{font-size:20px;margin:0}#speed{color:var(--mut);font-variant-numeric:tabular-nums}
form{display:flex;gap:8px;margin:6px 0 14px}input{flex:1;min-width:0;padding:12px;border-radius:12px;border:1px solid var(--line);background:var(--card);color:var(--fg);font-size:15px}
button{border:0;border-radius:12px;padding:11px 14px;font-size:15px;font-weight:600;background:var(--acc);color:#fff}button.s{background:transparent;color:var(--acc);border:1px solid var(--line);padding:8px 12px;font-size:14px}
button.d{color:var(--bad)}.bar2{display:flex;gap:8px;margin:0 0 12px}.card{background:var(--card);border:1px solid var(--line);border-radius:16px;padding:12px 14px;margin:0 0 10px}
.n{font-weight:600;word-break:break-word}.m{color:var(--mut);font-size:13px;margin-top:2px;font-variant-numeric:tabular-nums}.p{height:6px;background:var(--line);border-radius:6px;margin:9px 0 8px;overflow:hidden}
.p i{display:block;height:100%;background:var(--acc);border-radius:6px}.done .p i{background:var(--good)}.paused .p i{background:var(--warn)}.error .p i{background:var(--bad)}.a{display:flex;gap:8px;flex-wrap:wrap}
#msg{color:var(--mut);text-align:center;margin:30px 0}</style></head><body>
<header><h1>DDM</h1><span id="speed"></span></header>
<form id="f"><input id="u" type="url" placeholder="Paste a link to download" autocomplete="off"><button>Add</button></form>
<div class="bar2"><button class="s" data-all="pauseall">Pause all</button><button class="s" data-all="resumeall">Resume all</button></div>
<div id="list"></div><div id="msg">Loading…</div>
<script>
const key=new URLSearchParams(location.search).get('k')||'';
const api=(p,b)=>fetch(p,{method:b?'POST':'GET',headers:{'X-DDM-Key':key,'Content-Type':'application/json'},body:b?JSON.stringify(b):undefined,cache:'no-store'}).then(r=>{if(r.status==403)throw new Error('key');return r.json()});
const esc=s=>String(s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const kb=b=>{const k=b/1024;return (k>=1000?Math.round(k).toLocaleString('en-US'):k.toFixed(1))+' KB/s'};
const sz=b=>b<=0?'':b<1024?b+' bytes':b<1048576?(b/1024).toFixed(0)+' KB':b<1073741824?(b/1048576).toFixed(1)+' MB':(b/1073741824).toFixed(2)+' GB';
const tm=s=>s<0?'':s<60?s+' s':s<3600?Math.floor(s/60)+' min '+(s%60)+' s':Math.floor(s/3600)+' h '+Math.floor(s%3600/60)+' min';
function draw(d){document.getElementById('speed').textContent=d.speed>0?kb(d.speed):'';const L=document.getElementById('list');
document.getElementById('msg').textContent=d.items.length?'':'No downloads yet. Paste a link above.';
L.innerHTML=d.items.map(t=>{const pct=t.total>0?Math.min(100,t.done*100/t.total):0;const cls=t.state=='done'?'done':t.state=='paused'||t.state=='stopped'?'paused':t.state=='error'?'error':'';
const what=t.state=='running'?(t.total>0?pct.toFixed(1)+'% · ':'')+kb(t.speed)+(t.eta>=0?' · '+tm(t.eta)+' left':''):t.state=='done'?'Complete · '+sz(t.total):t.state=='error'?'Error'+(t.error?': '+t.error:''):t.state=='queued'?'Waiting':(t.state=='stopped'?'Stopped':'Paused')+(t.total>0?' · '+pct.toFixed(1)+'%':'');
const acts=t.state=='done'?'':(t.state=='running'||t.state=='queued'?'<button class="s" data-a="pause">Pause</button><button class="s" data-a="stop">Stop</button>':'<button class="s" data-a="resume">Resume</button>');
return '<div class="card '+cls+'" data-id="'+esc(t.id)+'"><div class="n">'+esc(t.name)+'</div><div class="p"><i style="width:'+pct+'%"></i></div><div class="m">'+esc(what)+(t.total>0&&t.state!='done'?' · '+sz(t.done)+' of '+sz(t.total):'')+'</div><div class="a" style="margin-top:8px">'+acts+'<button class="s d" data-a="remove">Remove</button></div></div>'}).join('')}
function load(){api('api/list').then(draw).catch(e=>{document.getElementById('msg').textContent=e.message=='key'?'This link is no longer valid. Scan the QR code in DDM again.':'DDM is not reachable. Is the PC on and on the same network?'})}
document.getElementById('f').onsubmit=e=>{e.preventDefault();const u=document.getElementById('u');if(!u.value.trim())return;api('api/do',{action:'add',url:u.value.trim()}).then(()=>{u.value='';setTimeout(load,800)})};
document.addEventListener('click',e=>{const b=e.target.closest('button');if(!b)return;if(b.dataset.all){api('api/do',{action:b.dataset.all}).then(load);return}
const c=b.closest('.card');if(!c||!b.dataset.a)return;if(b.dataset.a=='remove'&&!b.dataset.sure){b.dataset.sure=1;b.textContent='Tap again to remove';return}api('api/do',{action:b.dataset.a,id:c.dataset.id}).then(load)});
load();setInterval(load,1500);
</script></body></html>)HTML";

std::string stateName(const Task& t) {
    switch (t.state) {
        case State::Running: return "running";
        case State::Queued: return "queued";
        case State::Complete: return "done";
        case State::Error: return "error";
        default: return t.stopped ? "stopped" : "paused";
    }
}

// only numeric addresses (and localhost): a web page elsewhere can't reach DDM through a name it controls
bool hostOk(const std::string& host) {
    std::string h = lower(host);
    size_t c = h.rfind(':');
    if (c != std::string::npos) h = h.substr(0, c);
    if (h == "localhost") return true;
    int dots = 0;
    for (char ch : h) { if (ch == '.') dots++; else if (!isdigit(static_cast<unsigned char>(ch))) return false; }
    return dots == 3;
}

void handle(const web::Request& rq, web::Response& rs) {
    rs.headers["Content-Security-Policy"] = "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'";
    rs.headers["X-Frame-Options"] = "DENY";
    auto host = rq.headers.find("host");
    if (host == rq.headers.end() || !hostOk(host->second)) { rs.status = 403; rs.body = "forbidden"; return; }
    std::string key;
    { std::lock_guard<std::mutex> g(g_mu); key = settings().remoteKey; }
    {   // wrong keys: a few, then this address waits 10 minutes
        std::lock_guard<std::mutex> g(g_mu);
        if (g_fails.size() > 4096) g_fails.clear();   // (the list of addresses never grows without end)
        auto& f = g_fails[rq.peer];
        if (nowMs() - f.second > 10 * 60 * 1000) f = {0, nowMs()};
        if (f.first >= 20) { rs.status = 429; rs.body = "too many tries"; return; }
    }
    std::string given = rq.path == "/" ? rq.param("k") : (rq.headers.count("x-ddm-key") ? rq.headers.at("x-ddm-key") : "");
    if (!crypto::sameSecret(given, key)) {
        std::lock_guard<std::mutex> g(g_mu);
        g_fails[rq.peer].first++;
        rs.status = 403;
        rs.type = "text/html; charset=utf-8";
        rs.body = "<!doctype html><meta name=viewport content='width=device-width'><body style='font:16px sans-serif;padding:30px'>This link isn't valid any more. Open <b>Tools → Phone remote</b> in DDM and scan its QR code again.";
        return;
    }
    if (rq.path == "/" && rq.method == "GET") { rs.type = "text/html; charset=utf-8"; rs.body = kPage; return; }
    if (rq.path == "/api/list" && rq.method == "GET") {
        Json o = Json::object(), arr = Json::array();
        double speed = 0;
        // on the phone: what's downloading first, then the newest; the 300 latest are enough for a small screen
        auto all = Engine::get().snapshot();
        std::reverse(all.begin(), all.end());
        std::stable_partition(all.begin(), all.end(), [](const Task& t) { return t.state == State::Running || t.state == State::Queued; });
        int shown = 0;
        for (auto& t : all) {
            if (t.hidden) continue;
            if (++shown > 300) break;
            Json j = Json::object();
            j["id"] = t.id; j["name"] = t.fileName; j["state"] = stateName(t); j["kind"] = t.kind;
            j["done"] = static_cast<double>(t.done); j["total"] = static_cast<double>(t.total);
            j["speed"] = t.state == State::Running ? t.speed : 0.0; j["eta"] = static_cast<double>(t.state == State::Running ? t.eta : -1);
            if (t.state == State::Error) j["error"] = t.error;
            if (t.state == State::Running) speed += t.speed;
            arr.push(j);
        }
        o["items"] = arr;
        o["speed"] = speed;
        o["version"] = DDM_VERSION;
        rs.type = "application/json";
        rs.body = o.dump();
        return;
    }
    if (rq.path == "/api/do" && rq.method == "POST") {
        Json b = Json::parse(rq.body);
        std::string a = b["action"].str(), id = b["id"].str(), url = trim(b["url"].str());
        static const char* ok[] = {"add", "pause", "resume", "stop", "remove", "pauseall", "resumeall"};
        bool known = false;
        for (auto k : ok) if (a == k) known = true;
        if (!known) { rs.status = 400; rs.body = "{}"; return; }
        if (a == "add") {
            std::string l = lower(url);
            if (url.size() > 8192 || !(startsWith(l, "http://") || startsWith(l, "https://") || startsWith(l, "ftp://") || startsWith(l, "magnet:?"))) { rs.status = 400; rs.type = "application/json"; rs.body = "{\"error\":\"not a link\"}"; return; }
        } else if (a != "pauseall" && a != "resumeall") {
            Task t;
            if (!Engine::get().find(id, t)) { rs.status = 404; rs.body = "{}"; return; }
        }
        std::function<void(const std::string&, const std::string&, const std::string&)> h;
        { std::lock_guard<std::mutex> g(g_mu); h = g_handler; }
        if (h) h(a, id, url);
        rs.type = "application/json";
        rs.body = "{\"ok\":true}";
        return;
    }
    rs.status = 404;
    rs.body = "not found";
}
} // namespace

void setHandler(std::function<void(const std::string&, const std::string&, const std::string&)> fn) { std::lock_guard<std::mutex> g(g_mu); g_handler = fn; }

std::string newKey() {
    std::string k = crypto::b64(crypto::random(24), true, false);
    { std::lock_guard<std::mutex> g(g_mu); settings().remoteKey = k; g_fails.clear(); }
    saveSettings();
    return k;
}

bool start(std::string& error) {
    if (settings().remoteKey.size() < 20) newKey();
    int port = settings().remotePort > 0 && settings().remotePort < 65536 ? settings().remotePort : 8473;
    std::lock_guard<std::mutex> g(g_mu);
    if (g_server.running()) return true;
    if (!g_server.start(true, port, handle)) { error = "Port " + std::to_string(port) + " is in use by another program."; return false; }
    logLine("phone remote: on, port " + std::to_string(port));
    return true;
}

void stop() { std::lock_guard<std::mutex> g(g_mu); if (g_server.running()) { g_server.stop(); logLine("phone remote: off"); } }
bool running() { std::lock_guard<std::mutex> g(g_mu); return g_server.running(); }

std::vector<std::string> addresses() {
    std::vector<std::string> out;
    int port;
    std::string key;
    { std::lock_guard<std::mutex> g(g_mu); port = g_server.running() ? g_server.port() : settings().remotePort; key = settings().remoteKey; }
    for (auto& ip : web::localAddresses()) out.push_back("http://" + ip + ":" + std::to_string(port) + "/?k=" + key);
    return out;
}

} // namespace remote
} // namespace ddm
