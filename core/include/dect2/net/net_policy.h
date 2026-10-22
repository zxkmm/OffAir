// OffAir fork policy (not in upstream OnAir). Two rules, enforced at the lowest network calls so a new feature upstream adds
// on top of them is covered too:
//  - offline mode: no connection to a host outside this computer and the local network until the user turns internet access on
//    (View > Enable internet access). Off at every start, never saved.
//  - no automatic uploads: the feeds, spot reporters, the map and automatic log uploads never reach a host outside the local
//    network, whatever the internet switch says (a local program such as fr24feed on 127.0.0.1 still gets its data).
// Keep fork changes to upstream files as one-line hooks that call into this header, so merging upstream stays conflict-free.
#pragma once
#include <atomic>
#include <cctype>
#include <cstdio>
#include <initializer_list>
#include <string>

namespace dect2 {

// The GitHub repository the updater reads releases from: this fork's, so an update never replaces OffAir with stock OnAir.
inline constexpr const char* kOffAirRepo = "zxkmm/OffAir";

inline constexpr bool kOffAirNoUploads = true;   // the switches of the uploaders stay greyed out and off

inline std::atomic<bool> gInternetEnabled{false};   // offline at every start

inline void setInternetEnabled(bool on) { gInternetEnabled.store(on, std::memory_order_relaxed); }
inline bool internetEnabled() { return gInternetEnabled.load(std::memory_order_relaxed); }

inline bool privateIpv4(const std::string& h) {
    int p[4] = {-1, -1, -1, -1};
    char c = 0;
    if (sscanf(h.c_str(), "%d.%d.%d.%d%c", &p[0], &p[1], &p[2], &p[3], &c) != 4) return false;
    for (int i = 0; i < 4; i++) if (p[i] < 0 || p[i] > 255) return false;
    return p[0] == 10 || p[0] == 127 || (p[0] == 169 && p[1] == 254) || (p[0] == 192 && p[1] == 168) || (p[0] == 172 && p[1] >= 16 && p[1] <= 31);
}

// This computer or the local network: loopback, private IPv4, IPv6 loopback/link-local/unique-local literals, *.local and the
// other LAN-only suffixes, and a bare host name without dots. Anything else counts as the internet.
inline bool localHost(const std::string& host) {
    if (host.empty()) return false;
    std::string h = host;
    if (h.front() == '[' && h.back() == ']' && h.size() > 2) h = h.substr(1, h.size() - 2);
    for (char& c : h) c = (char)tolower((unsigned char)c);
    if (h == "localhost" || privateIpv4(h)) return true;
    if (h.find(':') != std::string::npos)   // an IPv6 literal
        return h == "::1" || h.rfind("fe80:", 0) == 0 || ((h[0] == 'f') && (h[1] == 'c' || h[1] == 'd') && h.find(':') <= 4);
    for (const char* suf : {".local", ".lan", ".home", ".home.arpa", ".internal", ".localdomain"}) {   // the usual LAN-only names
        const std::string x = suf;
        if (h.size() > x.size() && h.compare(h.size() - x.size(), x.size(), x) == 0) return true;
    }
    return h.find('.') == std::string::npos;
}

inline std::string urlHost(const std::string& url) {
    size_t p = url.find("://");
    p = p == std::string::npos ? 0 : p + 3;
    const size_t end = url.find_first_of("/?#", p);
    const size_t at = url.find('@', p);
    if (at != std::string::npos && at < end) p = at + 1;
    if (p >= url.size()) return {};
    if (url[p] == '[') {
        const size_t e = url.find(']', p + 1);
        return e == std::string::npos ? std::string() : url.substr(p + 1, e - (p + 1));
    }
    const size_t e = url.find_first_of(":/?#", p);
    return e == std::string::npos ? url.substr(p) : url.substr(p, e - p);
}

inline bool internetAllowedHost(const std::string& host) { return localHost(host) || internetEnabled(); }
inline bool fileUrl(const std::string& url) { return url.compare(0, 7, "file://") == 0; }   // a local file, not the network
inline bool internetAllowedUrl(const std::string& url) { return fileUrl(url) || internetAllowedHost(urlHost(url)); }

// automatic uploads: the local network only, the internet switch does not open them
inline bool uploadAllowedHost(const std::string& host) { return localHost(host); }
inline bool uploadAllowedUrl(const std::string& url) { return fileUrl(url) || uploadAllowedHost(urlHost(url)); }

inline constexpr const char* kOfflineError = "blocked: OffAir offline mode (View > Enable internet access)";
inline constexpr const char* kNoUploadError = "blocked: OffAir does not upload to the internet";

} // namespace dect2
