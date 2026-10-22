// Iridium bursts as gr-iridium iridium-extractor "RAW:" lines (see iridium_bits.h).
#include "dect2/common/u8file.h"
#include "dect2/iridium/iridium_bits.h"
#include "dect2/iridium/iridium_phy.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <chrono>
#include <cstring>
#include "../net/netcompat.h"
#include "dect2/net/net_policy.h"

namespace dect2 {

const char* const kIridiumRawRegex =
    R"(^(RAW|RWA|NC1): ([^ ]*) (-?[\d.]+) (\d+) (?:N:([+-]?\d+(?:\.\d+)?)([+-]\d+(?:\.\d+)?)|A:(\w+)) [IL]:(\w+) +(\d+)% ([\d.]+|inf|nan) +(\d+) ([\[\]<> 01]+)(.*)$)";

std::string iridiumRawLine(const IridiumBurstBits& b, long long t0, double unixTime, uint64_t id) {
    std::string bits = b.downlink ? iridium::kUwDlBits : iridium::kUwUlBits;
    bits.reserve(bits.size() + b.bits.size());
    for (uint8_t v : b.bits) bits.push_back(v ? '1' : '0');
    const double ms = std::max(0.0, (unixTime - (double)t0) * 1e3);
    // gr-iridium: magnitude = burst level over the noise floor's reference, noise = noise floor, both in dB; ours from the symbol
    // level and its SNR. Clamped so the printed field keeps its width and the parser's number pattern.
    const double mag = std::clamp(b.levelDb, -99.0, 99.0);
    const double noise = std::clamp(b.levelDb - b.snrDb, -99.0, 99.0);
    const double level = std::pow(10.0, b.levelDb / 20);
    const int conf = (int)std::lround(std::clamp(b.confidence, 0.0, 100.0));
    char head[256];
    snprintf(head, sizeof head, "RAW: i-%lld-t1 %012.4f %010d N:%05.2f%+06.2f I:%011llu %3d%% %.5f %3d ", t0, ms, (int)(b.freqHz + 0.5), mag, noise,
             (unsigned long long)id, conf, level, (int)(b.bits.size() / 2));
    return head + bits;
}

IridiumBitsWriter::~IridiumBitsWriter() { close(); closeStream(); }

bool IridiumBitsWriter::open(const std::string& path, std::string* err) {
    std::lock_guard<std::mutex> lk(mu_);
    if (f_) { fclose(f_); f_ = nullptr; }
    try {
        const std::filesystem::path p = dect2::u8file(path);
        if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
#ifdef _WIN32
        f_ = _wfopen(p.c_str(), L"wb");
#else
        f_ = fopen(p.c_str(), "wb");
#endif
    } catch (const std::exception&) { f_ = nullptr; }
    if (!f_) { if (err) *err = "cannot write " + path; return false; }
    path_ = path;
    t0_ = -1;
    lines_ = 0;
    return true;
}
void IridiumBitsWriter::close() {
    std::lock_guard<std::mutex> lk(mu_);
    if (f_) fclose(f_);
    f_ = nullptr;
}
bool IridiumBitsWriter::isOpen() const { std::lock_guard<std::mutex> lk(mu_); return f_ != nullptr; }
bool IridiumBitsWriter::active() const { std::lock_guard<std::mutex> lk(mu_); return f_ != nullptr || stream_; }
bool IridiumBitsWriter::streamOpen() const { std::lock_guard<std::mutex> lk(mu_); return stream_; }
uint64_t IridiumBitsWriter::streamLines() const { std::lock_guard<std::mutex> lk(mu_); return streamed_; }

static double nowSec() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

static sock_t makeSock(bool tcp, const sockaddr_in& to) {
    netInit();
    sock_t s = socket(AF_INET, tcp ? SOCK_STREAM : SOCK_DGRAM, 0);
    if (s == kBadSock) return kBadSock;
#ifdef SO_NOSIGPIPE
    int one = 1; setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, (const char*)&one, sizeof one);
#endif
    if (tcp) {
        // short, bounded connect: a dead listener must not stall the decoder
#ifdef _WIN32
        DWORD tmo = 300; setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tmo, sizeof tmo);
#else
        timeval tv{0, 300000}; setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof tv);
#endif
        if (connect(s, (const sockaddr*)&to, sizeof to) != 0) { sockClose(s); return kBadSock; }
    }
    return s;
}

bool IridiumBitsWriter::openStream(bool tcp, const std::string& host, int port, std::string* err) {
    std::lock_guard<std::mutex> lk(mu_);
    if (sock_ >= 0) { sockClose((sock_t)sock_); sock_ = -1; }
    stream_ = false;
    if (port < 1 || port > 65535) { if (err) *err = "port must be 1 to 65535"; return false; }
    sockaddr_in a{};
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port);
    const std::string h = host.empty() ? "127.0.0.1" : host;
    if (!dect2::internetAllowedHost(h)) { if (err) *err = dect2::kOfflineError; return false; }   // OffAir
    if (inet_pton(AF_INET, h.c_str(), &a.sin_addr) != 1) { if (err) *err = "host must be an IPv4 address"; return false; }
    tcp_ = tcp; host_ = h; port_ = port; retryAt_ = 0; streamed_ = 0; stream_ = true;
    if (!tcp) {
        const sock_t s = makeSock(false, a);
        if (s == kBadSock) { stream_ = false; if (err) *err = "cannot open the UDP socket"; return false; }
        sock_ = (int)s;
    }   // TCP connects on the first line and again after a drop
    return true;
}
void IridiumBitsWriter::closeStream() {
    std::lock_guard<std::mutex> lk(mu_);
    if (sock_ >= 0) sockClose((sock_t)sock_);
    sock_ = -1; stream_ = false;
}
// mu_ held
void IridiumBitsWriter::streamSend(const std::string& line) {
    if (!stream_) return;
    sockaddr_in a{};
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port_); inet_pton(AF_INET, host_.c_str(), &a.sin_addr);
    if (tcp_ && sock_ < 0) {
        if (nowSec() < retryAt_) return;
        const sock_t s = makeSock(true, a);
        if (s == kBadSock) { retryAt_ = nowSec() + 5; return; }
        sock_ = (int)s;
    }
    if (sock_ < 0) return;
    long long r;
    if (tcp_) {
#ifdef MSG_NOSIGNAL
        r = send((sock_t)sock_, line.data(), line.size(), MSG_NOSIGNAL);
#else
        r = sockSend((sock_t)sock_, line.data(), line.size());
#endif
        if (r != (long long)line.size()) { sockClose((sock_t)sock_); sock_ = -1; retryAt_ = nowSec() + 5; return; }
    } else {
        r = sockSendTo((sock_t)sock_, line.data(), line.size(), a);
        if (r < 0) return;
    }
    streamed_++;
}
uint64_t IridiumBitsWriter::lines() const { std::lock_guard<std::mutex> lk(mu_); return lines_; }
std::string IridiumBitsWriter::path() const { std::lock_guard<std::mutex> lk(mu_); return path_; }

void IridiumBitsWriter::write(const IridiumBurstBits& b, double unixTime) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!f_ && !stream_) return;
    if (t0_ < 0) t0_ = (long long)std::floor(unixTime);   // gr-iridium: the whole second of the first burst
    std::string s = iridiumRawLine(b, t0_, unixTime, lines_);
    s.push_back('\n');
    if (f_) fputs(s.c_str(), f_);
    streamSend(s);
    if ((++lines_ & 31) == 0 && f_) fflush(f_);
}

} // namespace dect2
