// A small HTTP server of DDM's own (Winsock): the phone remote (on your home network, with a secret key) and
// "Play while downloading" (on this PC only). One thread per connection, at most 24 at a time, requests up to 64 KB.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ddm {
namespace web {

struct Request {
    std::string method, path, query, body, peer;   // path without the query; peer = the caller's IP address
    std::map<std::string, std::string> headers;     // lower-case names
    std::string param(const std::string& name) const;   // from the query (URL-decoded)
};

// A response writes itself: either body + type, or stream (then the handler writes everything itself).
struct Response {
    int status = 200;
    std::string type = "text/plain; charset=utf-8";
    std::string body;
    std::map<std::string, std::string> headers;
    std::function<void(std::function<bool(const char*, size_t)> send)> stream;   // send() false = the caller went away
};

class Server {
public:
    ~Server() { stop(); }
    // bindAll: every network of this PC (else 127.0.0.1 only). port 0 = any free port. false = couldn't listen.
    bool start(bool bindAll, int port, std::function<void(const Request&, Response&)> handler);
    void stop();
    int port() const { return port_; }
    bool running() const { return listener_ != ~static_cast<uintptr_t>(0); }
private:
    uintptr_t listener_ = ~static_cast<uintptr_t>(0);
    int port_ = 0;
    std::shared_ptr<std::atomic<bool>> alive_;
};

std::string urlDecode(const std::string& s);
std::string htmlEscape(const std::string& s);
std::string statusText(int code);
// IPv4 addresses of this PC on its networks (Wi-Fi / Ethernet that are up), best first.
std::vector<std::string> localAddresses();

} // namespace web
} // namespace ddm
