#pragma once
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <string>

namespace dect2 {

inline std::atomic<bool> gInternetEnabled{false};   // default offline at startup

inline void setInternetEnabled(bool on) { gInternetEnabled.store(on, std::memory_order_relaxed); }
inline bool internetEnabled() { return gInternetEnabled.load(std::memory_order_relaxed); }

inline bool privateIpv4(const std::string& h) {
    int p[4] = {-1, -1, -1, -1};
    char c = 0;
    if (sscanf(h.c_str(), "%d.%d.%d.%d%c", &p[0], &p[1], &p[2], &p[3], &c) != 4) return false;
    for (int i = 0; i < 4; i++) if (p[i] < 0 || p[i] > 255) return false;
    return p[0] == 10 || p[0] == 127 || (p[0] == 169 && p[1] == 254) || (p[0] == 192 && p[1] == 168) || (p[0] == 172 && p[1] >= 16 && p[1] <= 31);
}

inline bool localHost(const std::string& host) {
    if (host.empty()) return false;
    std::string h = host;
    if (h.front() == '[' && h.back() == ']' && h.size() > 2) h = h.substr(1, h.size() - 2);
    for (char& c : h) c = (char)tolower((unsigned char)c);
    if (h == "localhost" || h == "::1") return true;
    if (privateIpv4(h)) return true;
    if (h.rfind("fe80:", 0) == 0 || h.rfind("fc", 0) == 0 || h.rfind("fd", 0) == 0) return true;
    if (h.size() >= 6 && h.compare(h.size() - 6, 6, ".local") == 0) return true;
    const bool hasDot = h.find('.') != std::string::npos;
    const bool hasColon = h.find(':') != std::string::npos;
    if (!hasDot && !hasColon) return true;   // likely a LAN host name
    return false;
}

inline std::string urlHost(const std::string& url) {
    size_t p = url.find("://");
    p = p == std::string::npos ? 0 : p + 3;
    const size_t at = url.find('@', p);
    if (at != std::string::npos) p = at + 1;
    if (p >= url.size()) return {};
    if (url[p] == '[') {
        const size_t e = url.find(']', p + 1);
        return e == std::string::npos ? std::string() : url.substr(p + 1, e - (p + 1));
    }
    const size_t e = url.find_first_of(":/?#", p);
    return e == std::string::npos ? url.substr(p) : url.substr(p, e - p);
}

inline bool internetAllowedHost(const std::string& host) { return internetEnabled() || localHost(host); }
inline bool internetAllowedUrl(const std::string& url) {
    const std::string h = urlHost(url);
    return !h.empty() && internetAllowedHost(h);
}

} // namespace dect2
