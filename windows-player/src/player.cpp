#include "player.h"

#include <iphlpapi.h>
#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <cstring>
#include <string>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

namespace ahub {

// ------------------------------------------------------------------
// 软限幅
// ------------------------------------------------------------------
/**
 * 拐点取 0.95 满刻度：单个源几乎不被压缩，只有多源叠加超出时才介入。
 */
static inline int16_t softLimit(int32_t x) {
    const float T    = 0.95f * 32767.0f;
    const float room = 32767.0f - T;
    float f = (float)x;
    if (f > T || f < -T) {
        float sgn  = (f > 0.0f) ? 1.0f : -1.0f;
        float a    = std::fabs(f);
        float over = a - T;
        a = T + room * (over / (over + room));
        f = sgn * a;
    }
    if (f >  32767.0f) f =  32767.0f;
    if (f < -32768.0f) f = -32768.0f;
    return (int16_t)f;
}

// ------------------------------------------------------------------
// 扫描辅助
// ------------------------------------------------------------------
/**
 * 确保 Winsock 已初始化。
 *
 * ⚠️ 必须在这里做，不能只在 Player::init() 里做：
 *    runNetwork() 是先调用 Player::scan()（出站扫描），
 *    再调用 player.init()。如果 WSAStartup 只在 init 里调用，
 *    扫描时 socket() 会因为 WSANOTINITIALISED 全部失败，
 *    表现为"扫描了 254 个地址但一个都发现不了"且没有任何报错。
 *    —— 这个坑真实发生过。
 */
static bool ensureWsa() {
    static std::mutex m;
    static bool inited = false;
    std::lock_guard<std::mutex> lk(m);
    if (!inited) {
        WSADATA wsa;
        inited = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    }
    return inited;
}

static bool tryConnect(const std::string& ip, uint16_t port, int timeoutMs,
                       const std::string& localIp) {
    if (!ensureWsa()) return false;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    if (!localIp.empty()) {
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = inet_addr(localIp.c_str());
        if (local.sin_addr.s_addr == INADDR_NONE
                || bind(s, (sockaddr*)&local, sizeof(local)) != 0) {
            closesocket(s);
            return false;
        }
    }
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);

    sockaddr_in a{};
    a.sin_family      = AF_INET;
    a.sin_port        = htons(port);
    a.sin_addr.s_addr = inet_addr(ip.c_str());

    bool ok = false;
    int r = connect(s, (sockaddr*)&a, sizeof(a));
    if (r == 0) {
        ok = true;
    } else if (WSAGetLastError() == WSAEWOULDBLOCK) {
        fd_set wf; FD_ZERO(&wf); FD_SET(s, &wf);
        timeval tv;
        tv.tv_sec  = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        int sel = select(0, nullptr, &wf, nullptr, &tv);
        if (sel > 0) {
            int err = 0, len = sizeof(err);
            getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &len);
            ok = (err == 0);
        }
    }
    if (ok) {
        // A TCP accept alone does not identify an AudioHub sender. VPNs, proxies,
        // and other LAN services can accept probes for many addresses. Require
        // the sender's first complete HELLO packet before adding a device.
        nb = 0;
        if (ioctlsocket(s, FIONBIO, &nb) != 0) ok = false;
        int recvTimeout = std::max(timeoutMs, 250);
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                   (const char*)&recvTimeout, sizeof(recvTimeout));
        auto readExact = [&](uint8_t* dst, int count) {
            int got = 0;
            while (got < count) {
                int n = recv(s, (char*)dst + got, count - got, 0);
                if (n <= 0) return false;
                got += n;
            }
            return true;
        };
        PktHeader hello{};
        if (ok) ok = readExact((uint8_t*)&hello, (int)sizeof(hello));
        if (ok) ok = memcmp(hello.magic, MAGIC, sizeof(MAGIC)) == 0
                && hello.version == PROTO_VER && hello.type == PKT_HELLO
                && hello.bodyLen >= 3 && hello.bodyLen <= 64;
        uint8_t body[64]{};
        if (ok) ok = readExact(body, (int)hello.bodyLen);
        if (ok) {
            const int nameLen = body[2];
            const bool legacyHello = hello.bodyLen == (uint32_t)(3 + nameLen);
            const bool qualityHello = hello.bodyLen == (uint32_t)(4 + nameLen)
                    && body[3 + nameLen] <= 2;
            ok = (body[0] == CODEC_PCM)
                    && body[1] < BW_COUNT && (legacyHello || qualityHello);
        }
    }
    closesocket(s);
    return ok;
}

namespace {

struct LocalIpCandidate {
    std::string ip;
    ULONG ifType = IF_TYPE_OTHER;
    bool virtualAdapter = false;
};

static bool looksLikeVpnAdapter(const IP_ADAPTER_ADDRESSES* a) {
    std::wstring text;
    if (a->FriendlyName) text += a->FriendlyName;
    if (a->Description) {
        text += L" ";
        text += a->Description;
    }
    for (wchar_t& ch : text) ch = (wchar_t)towlower(ch);
    static const wchar_t* names[] = {
        L"vpn", L"tun", L"tap", L"wintun", L"wireguard", L"zerotier",
        L"tailscale", L"hamachi", L"nordlynx", L"clash", L"virtual"
    };
    for (const wchar_t* name : names) {
        if (text.find(name) != std::wstring::npos) return true;
    }
    return a->IfType == IF_TYPE_TUNNEL || a->IfType == IF_TYPE_PPP;
}

static std::string readWindowsClipboard() {
    std::string out;
    if (!OpenClipboard(nullptr)) return out;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const wchar_t* w = (const wchar_t*)GlobalLock(h);
        if (w) {
            int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
            if (n > 1) {
                std::vector<char> bytes((size_t)n);
                WideCharToMultiByte(CP_UTF8, 0, w, -1, bytes.data(), n, nullptr, nullptr);
                out.assign(bytes.data(), (size_t)n - 1);
            }
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return out;
}

static bool writeWindowsClipboard(const std::string& text) {
    bool opened = false;
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (OpenClipboard(nullptr)) { opened = true; break; }
        Sleep(15);
    }
    if (!opened) return false;
    if (!EmptyClipboard()) { CloseClipboard(); return false; }
    bool ok = false;
    int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)(n + 1) * sizeof(wchar_t));
    if (h) {
        wchar_t* w = (wchar_t*)GlobalLock(h);
        if (w) {
            if (n > 0) MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), w, n);
            w[n] = L'\0';
            GlobalUnlock(h);
            if (SetClipboardData(CF_UNICODETEXT, h)) ok = true;
            else GlobalFree(h);
        } else {
            GlobalFree(h);
        }
    }
    CloseClipboard();
    return ok;
}

static std::vector<LocalIpCandidate> localIpCandidates() {
    std::vector<LocalIpCandidate> out;
    ULONG size = 15000;
    std::vector<uint8_t> buf(size);
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;

    ULONG r = GetAdaptersAddresses(AF_INET, flags, nullptr,
                                  (IP_ADAPTER_ADDRESSES*)buf.data(), &size);
    if (r == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        r = GetAdaptersAddresses(AF_INET, flags, nullptr,
                                 (IP_ADAPTER_ADDRESSES*)buf.data(), &size);
    }
    if (r != NO_ERROR) return out;

    for (IP_ADAPTER_ADDRESSES* a = (IP_ADAPTER_ADDRESSES*)buf.data(); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        const bool virtualAdapter = looksLikeVpnAdapter(a);
        for (IP_ADAPTER_UNICAST_ADDRESS* ua = a->FirstUnicastAddress; ua; ua = ua->Next) {
            if (!ua->Address.lpSockaddr) continue;
            if (ua->Address.lpSockaddr->sa_family != AF_INET) continue;
            sockaddr_in* sin = (sockaddr_in*)ua->Address.lpSockaddr;
            const char* p = inet_ntoa(sin->sin_addr);
            if (!p) continue;
            std::string ip = p;
            if (ip.rfind("127.", 0) == 0) continue;
            if (ip.rfind("169.254.", 0) == 0) continue;
            out.push_back({ip, a->IfType, virtualAdapter});
        }
    }
    std::sort(out.begin(), out.end(), [](const LocalIpCandidate& a, const LocalIpCandidate& b) {
        auto adapterRank = [](ULONG type) {
            // WiFi 优先，其次物理有线网卡；VPN/TUN/虚拟网卡放到最后。
            if (type == IF_TYPE_IEEE80211) return 0;
            if (type == IF_TYPE_ETHERNET_CSMACD) return 1;
            return 2;
        };
        auto prefixRank = [](const std::string& ip) {
            if (ip.rfind("192.168.", 0) == 0) return 0;
            if (ip.rfind("10.", 0) == 0) return 1;
            if (ip.rfind("172.", 0) == 0) return 2;
            return 3;
        };
        const int ar = (a.virtualAdapter ? 3 : adapterRank(a.ifType)) * 10 + prefixRank(a.ip);
        const int br = (b.virtualAdapter ? 3 : adapterRank(b.ifType)) * 10 + prefixRank(b.ip);
        if (ar != br) return ar < br;
        return a.ip < b.ip;
    });
    out.erase(std::unique(out.begin(), out.end(), [](const LocalIpCandidate& a,
                                                      const LocalIpCandidate& b) {
        return a.ip == b.ip;
    }), out.end());
    return out;
}

} // namespace

std::vector<std::string> Player::localIPv4() {
    std::vector<std::string> out;
    for (const auto& c : localIpCandidates()) out.push_back(c.ip);
    return out;
}

void Player::setPreferredIp(const std::string& ip) {
    if (ip.empty()) return;
    const auto ips = localIPv4();
    if (std::find(ips.begin(), ips.end(), ip) != ips.end()) preferredIp_ = ip;
}

std::vector<std::string> Player::localSubnetHosts() const {
    std::vector<std::string> out;
    // 只扫描一个优选的物理局域网网段，避免 VPN/TUN 网卡带来额外的扫描
    // （例如 172.* 网段可能让启动时出现数百个无关设备）。
    std::string selected = preferredIp_;
    if (selected.empty()) {
        const auto locals = localIPv4();
        if (!locals.empty()) selected = locals.front();
    }
    if (selected.empty()) return out;

    size_t p1 = selected.find('.');
    size_t p2 = (p1 == std::string::npos) ? std::string::npos : selected.find('.', p1 + 1);
    size_t p3 = (p2 == std::string::npos) ? std::string::npos : selected.find('.', p2 + 1);
    if (p3 == std::string::npos) return out;
    std::string prefix = selected.substr(0, p3);
    for (int i = 1; i <= 254; i++) {
        char h[64];
        sprintf(h, "%s.%d", prefix.c_str(), i);
        out.push_back(h);
    }
    return out;
}

std::vector<std::string> Player::scanNew(uint16_t port, int timeoutMs, int* scanned,
                                          const std::atomic<bool>* keepRunning) {
    std::vector<std::string> hosts = localSubnetHosts();
    hosts.erase(std::remove(hosts.begin(), hosts.end(), preferredIp_), hosts.end());

    // 排除已经建立连接的地址：重复探测会打断正在工作的接收端
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto* s : sources_) {
            hosts.erase(std::remove(hosts.begin(), hosts.end(), s->host), hosts.end());
        }
    }
    if (scanned) *scanned = (int)hosts.size();

    std::vector<std::string> found;
    std::mutex foundMtx;
    std::atomic<size_t> idx{0};
    const uint32_t deadline = GetTickCount() + 5000u;

    auto worker = [&]() {
        while (true) {
            if (keepRunning && !keepRunning->load()) break;
            if ((int32_t)(GetTickCount() - deadline) >= 0) break;
            size_t i = idx.fetch_add(1);
            if (i >= hosts.size()) break;
            if (tryConnect(hosts[i], port, timeoutMs, preferredIp_)) {
                std::lock_guard<std::mutex> lk(foundMtx);
                found.push_back(hosts[i]);
            }
            Sleep(20);
        }
    };

    unsigned n = std::min(16u, (unsigned)hosts.size());
    if (n == 0) { return found; }
    std::vector<std::thread> ts;
    for (unsigned i = 0; i < n; i++) ts.emplace_back(worker);
    for (auto& t : ts) t.join();

    std::sort(found.begin(), found.end());
    return found;
}

std::vector<std::string> Player::scan(uint16_t port, int timeoutMs, int* scanned) {
    std::vector<std::string> hosts = localSubnetHosts();
    hosts.erase(std::remove(hosts.begin(), hosts.end(), preferredIp_), hosts.end());
    if (scanned) *scanned = (int)hosts.size();

    std::vector<std::string> found;
    std::mutex foundMtx;
    std::atomic<size_t> idx{0};

    auto worker = [&]() {
        while (true) {
            size_t i = idx.fetch_add(1);
            if (i >= hosts.size()) break;
            if (tryConnect(hosts[i], port, timeoutMs, preferredIp_)) {
                std::lock_guard<std::mutex> lk(foundMtx);
                found.push_back(hosts[i]);
            }
            Sleep(20);
        }
    };

    unsigned n = std::min(4u, (unsigned)hosts.size());
    std::vector<std::thread> ts;
    for (unsigned i = 0; i < n; i++) ts.emplace_back(worker);
    for (auto& t : ts) t.join();

    std::sort(found.begin(), found.end());
    return found;
}

// ------------------------------------------------------------------
Player::~Player() { shutdown(); }

void Player::setErr(Source* s, const std::string& msg) {
    std::lock_guard<std::mutex> lk(s->errMtx);
    s->err = msg;
}

bool Player::init(const std::vector<std::string>& hosts, uint16_t port, std::string& err) {
    if (!ensureWsa()) { err = "WSAStartup 失败"; return false; }
    if (preferredIp_.empty()) {
        const auto ips = localIPv4();
        if (!ips.empty()) preferredIp_ = ips.front();
    }
    // 图形界面的服务由用户点击开始；命令行传入的 hosts 保持原有行为。
    if (!hosts.empty()) {
        if (!startReceiverService(err)) return false;
        for (const auto& h : hosts) addHost(h, port);
    }
    return true;
}

bool Player::startSenderService(std::string& err) {
    if (clipboardServer_.isListening()) return true;
    if (!clipboardServer_.start(preferredIp_, DISCOVERY_PORT, announceName_,
            [this](const std::string& text) { receiveClipboard(text); }, err)) {
        senderListenError_ = err;
        return false;
    }
    senderListenError_.clear();
    return true;
}

void Player::stopSenderService() {
    loopbackSender_.stop();
    clipboardServer_.stop();
}

bool Player::startReceiverService(std::string& err) {
    (void)err;
    if (announceRunning_.load()) return true;
    announceRunning_ = true;
    announceThread_ = std::thread(&Player::announceLoop, this);
    statsRunning_ = true;
    statsThread_ = std::thread(&Player::statsLoop, this);
    return true;
}

void Player::stopReceiverService() {
    announceRunning_ = false;
    if (announceThread_.joinable()) announceThread_.join();
    statsRunning_ = false;
    if (statsThread_.joinable()) statsThread_.join();
    std::vector<Source*> snapshot;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        snapshot = sources_;
    }
    for (auto* s : snapshot) {
        s->running = false;
        SOCKET sk = s->sock.load();
        if (sk != INVALID_SOCKET) ::shutdown(sk, SD_BOTH);
    }
    for (auto* s : snapshot) if (s->th.joinable()) s->th.join();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        sources_.clear();
        acc_.clear();
    }
    for (auto* s : snapshot) delete s;
}

/**
 * 周期性 UDP 广播本机存在。
 *
 * 连接方向是"接收端主动连发送端"，所以手机（发送端）无法主动发现本播放端；
 * 不做这个广播，手机的『发送』界面上就永远看不到电脑。
 * UDP 是出站发送，不受 Windows 防火墙入站规则影响。
 *
 * 只向优选物理网卡对应的 /24 定向广播发送，避免 VPN/虚拟网卡收到无关广播。
 */
void Player::announceLoop() {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return;
    BOOL yes = TRUE;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char*)&yes, sizeof(yes));

    // 绑定到优选的 WiFi/有线地址，避免有限广播被系统路由到 VPN/TUN 网卡。
    if (!preferredIp_.empty()) {
        sockaddr_in bindAddr{};
        bindAddr.sin_family = AF_INET;
        bindAddr.sin_addr.s_addr = inet_addr(preferredIp_.c_str());
        bindAddr.sin_port = 0;
        if (bind(s, (const sockaddr*)&bindAddr, sizeof(bindAddr)) != 0) {
            closesocket(s);
            return;
        }
    }

    // 组包：[通用包头][UTF-8 设备名]
    const std::string nm = announceName_;
    std::vector<uint8_t> pkt(sizeof(PktHeader) + nm.size());
    PktHeader* h = (PktHeader*)pkt.data();
    memcpy(h->magic, MAGIC, 4);
    h->version  = PROTO_VER;
    h->type     = PKT_ANNOUNCE;
    h->flags    = 0;
    h->sourceId = 0;
    h->bodyLen  = (uint32_t)nm.size();
    if (!nm.empty()) memcpy(pkt.data() + sizeof(PktHeader), nm.data(), nm.size());

    // 目标：只向优选局域网网段发送定向广播；不再向 255.255.255.255 泛洪。
    std::vector<uint32_t> targets;
    if (!preferredIp_.empty()) {
        // MinGW 8.1 的头文件里没有 inet_pton，用 inet_addr
        unsigned long a = inet_addr(preferredIp_.c_str());
        if (a != INADDR_NONE) {
            uint32_t host = ntohl(a);
            targets.push_back(htonl((host & 0xFFFFFF00u) | 0xFFu));   // 假设 /24
        }
    } else {
        targets.push_back(INADDR_BROADCAST);
    }

    while (announceRunning_.load()) {
        for (uint32_t t : targets) {
            sockaddr_in to{};
            to.sin_family      = AF_INET;
            to.sin_port        = htons(ANNOUNCE_PORT);
            to.sin_addr.s_addr = t;
            sendto(s, (const char*)pkt.data(), (int)pkt.size(), 0,
                   (const sockaddr*)&to, sizeof(to));
        }
        for (int i = 0; i < 50 && announceRunning_.load(); i++) Sleep(100);
    }
    PktHeader bye{};
    memcpy(bye.magic, MAGIC, 4);
    bye.version = PROTO_VER;
    bye.type = PKT_BYE;
    for (uint32_t t : targets) {
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(ANNOUNCE_PORT);
        to.sin_addr.s_addr = t;
        sendto(s, (const char*)&bye, (int)sizeof(bye), 0,
               (const sockaddr*)&to, sizeof(to));
    }
    closesocket(s);
}

bool Player::addHost(const std::string& host, uint16_t port) {
    if (!ensureWsa()) return false;
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto* s : sources_) {
        if (s->host == host && s->port == port) return false;   // 已存在
    }
    Source* s = new Source();
    s->host = host;
    s->port = port;
    s->running = true;
    sources_.push_back(s);
    s->th = std::thread(&Player::sourceLoop, this, s);
    return true;
}

void Player::shutdown() {
    stopReceiverService();
    stopSenderService();
    WSACleanup();
    std::lock_guard<std::mutex> lk(mtx_);
    acc_.clear();
}

int Player::connectedSources() {
    std::lock_guard<std::mutex> lk(mtx_);
    int n = 0;
    for (auto* s : sources_) if (s->connected.load()) n++;
    return n;
}

bool Player::hasRecentAudio() {
    std::lock_guard<std::mutex> lk(mtx_);
    const uint32_t now = GetTickCount();
    for (auto* s : sources_) {
        if (s->connected.load() && s->remoteStreamEnabled.load()
                && s->lastRecvMs.load() != 0
                && now - s->lastRecvMs.load() < 1500u) return true;
    }
    return false;
}

void Player::receiveClipboard(const std::string& text) {
    {
        std::lock_guard<std::mutex> lk(clipboardMtx_);
        clipboardText_ = text;
    }
    writeWindowsClipboard(text);
    clipboardReceiveSeq_.fetch_add(1);
}

std::string Player::clipboardText() const {
    std::lock_guard<std::mutex> lk(clipboardMtx_);
    return clipboardText_;
}

bool Player::copyReceivedClipboard() const {
    const std::string text = clipboardText();
    if (text.empty()) return false;
    return writeWindowsClipboard(text);
}

int Player::sendClipboard() {
    const std::string text = readWindowsClipboard();
    int sent = clipboardServer_.send(text);
    std::lock_guard<std::mutex> sourcesLock(mtx_);
    for (auto* s : sources_) {
        if (!s->connected.load()) continue;
        std::vector<uint8_t> pkt = buildClipboard(text);
        std::lock_guard<std::mutex> lk(s->txMtx);
        SOCKET sk = s->sock.load();
        if (sk == INVALID_SOCKET) continue;
        size_t off = 0;
        bool ok = true;
        while (off < pkt.size()) {
            int n = send(sk, (const char*)pkt.data() + off,
                         (int)(pkt.size() - off), 0);
            if (n <= 0) { ok = false; break; }
            off += (size_t)n;
        }
        if (ok) sent++;
    }
    return sent;
}

bool Player::startAudioSender(std::string& err) {
    if (!clipboardServer_.isListening()) {
        err = senderListenError_.empty() ? "电脑发送服务未启动" : senderListenError_;
        return false;
    }
    return loopbackSender_.start([this](const int16_t* pcm, int frames) {
        clipboardServer_.sendAudio(pcm, frames);
    }, [this]() { return clipboardServer_.peerCount() > 0; }, err);
}

// ------------------------------------------------------------------
// 每个源：连接 → 收流 → 重连
// ------------------------------------------------------------------
void Player::sourceLoop(Source* s) {
    std::vector<uint8_t> buf((size_t)CLIPBOARD_MAX_BYTES + sizeof(PktHeader) + 32);
    size_t fill = 0;

    while (s->running) {
        SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == INVALID_SOCKET) { Sleep(1000); continue; }

        if (!preferredIp_.empty()) {
            sockaddr_in local{};
            local.sin_family = AF_INET;
            local.sin_addr.s_addr = inet_addr(preferredIp_.c_str());
            if (local.sin_addr.s_addr == INADDR_NONE
                    || bind(sock, (sockaddr*)&local, sizeof(local)) != 0) {
                setErr(s, "选定的局域网地址不可用，正在重试");
                closesocket(sock);
                for (int i = 0; i < 50 && s->running; i++) Sleep(100);
                continue;
            }
        }

        BOOL nodelay = TRUE;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));
        int rcvBuf = 256 * 1024;
        setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (const char*)&rcvBuf, sizeof(rcvBuf));

        sockaddr_in a{};
        a.sin_family      = AF_INET;
        a.sin_port        = htons(s->port);
        a.sin_addr.s_addr = inet_addr(s->host.c_str());

        u_long nonblocking = 1;
        ioctlsocket(sock, FIONBIO, &nonblocking);
        int rc = connect(sock, (sockaddr*)&a, sizeof(a));
        bool connectedNow = rc == 0;
        if (!connectedNow && WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set writable;
            FD_ZERO(&writable);
            FD_SET(sock, &writable);
            timeval timeout{0, 500000};
            if (select(0, nullptr, &writable, nullptr, &timeout) > 0) {
                int socketError = 0, errorSize = sizeof(socketError);
                connectedNow = getsockopt(sock, SOL_SOCKET, SO_ERROR,
                        (char*)&socketError, &errorSize) == 0 && socketError == 0;
            }
        }
        nonblocking = 0;
        ioctlsocket(sock, FIONBIO, &nonblocking);
        if (!connectedNow || !s->running) {
            setErr(s, "连接失败，正在重试");
            closesocket(sock);
            for (int i = 0; i < 50 && s->running; i++) Sleep(100);
            continue;
        }
        s->sock.store(sock);   // 供延迟上报线程使用

        {
            std::lock_guard<std::mutex> lk(mtx_);
            s->jb.reset();
            s->fifo.clear();
            s->fifoPos = 0;
            s->level = BW_FULL;
            s->quality = 1;
            s->samples = FRAME_SAMPLES;
            s->channels = CHANNELS;
            s->packetBytes = AUDIO_PKT_BYTES;
        }
        s->lastRecvMs.store(0);
        s->minOffsetUs = LLONG_MAX;
        s->connected = true;
        s->remoteStreamEnabled = true;
        fill = 0;
        setErr(s, "");

        // 返回 true 表示消费了数据；返回 false 表示数据不够、需要再 recv。
        // ⚠️ 必须返回这个标志：早期版本在"包没收全"时直接 return 且不消费任何字节，
        //    而外层 while 又立刻再调用一次，形成 100% CPU 死循环，接收线程彻底卡死。
        auto handleOne = [&]() -> bool {
            // 重新同步：magic 或版本不对时向前找下一个包头
            if (memcmp(buf.data(), MAGIC, 4) != 0 || buf[4] != PROTO_VER) {
                bool found = false;
                for (size_t i = 1; i + 5 <= fill; i++) {
                    if (memcmp(buf.data() + i, MAGIC, 4) == 0 && buf[i + 4] == PROTO_VER) {
                        memmove(buf.data(), buf.data() + i, fill - i);
                        fill -= i;
                        found = true;
                        break;
                    }
                }
                if (!found) fill = 0;
                return true;
            }

            const PktHeader* h = (const PktHeader*)buf.data();
            const int hdr = (int)sizeof(PktHeader);
            if (h->bodyLen > buf.size() - (size_t)hdr) { fill = 0; return true; }
            const int need = hdr + (int)h->bodyLen;
            if (fill < (size_t)need) return false;

            // ---- HELLO：编码方式 + 带宽档位 + 设备名 ----
            if (h->type == PKT_HELLO) {
                if (h->bodyLen < 3) { fill = 0; return true; }
                int lv = helloLevel(buf.data(), (int)fill);
                int q  = helloQuality(buf.data(), (int)fill);
                std::string nm = helloName(buf.data(), (int)fill);

                {
                    std::lock_guard<std::mutex> lk(mtx_);
                    applySourceFormat(s, lv);
                    if (q >= 0) s->quality = q;
                    if (s->jb) s->jb->setTargetMs(s->quality == 0 ? 20
                            : s->quality == 2 ? 80 : 40);
                }
                if (!nm.empty()) {
                    setDevName(s, nm);
                    char msg[256];
                    sprintf(msg, "%s（%s，%d kbps）", nm.c_str(), bwLabel(lv), bwKbps(lv));
                    setDevMsg(s, msg);
                }
                memmove(buf.data(), buf.data() + need, fill - need);
                fill -= need;
                return true;
            }

            // ---- AUTH：发送端的授权状态 ----
            // 连接方向是"接收端主动连发送端"，发送端默认不会立刻发音频，
            // 需要它本人在界面上点【允许】。这里如实把状态显示给用户。
            if (h->type == PKT_AUTH) {
                if (h->bodyLen < 1) { fill = 0; return true; }
                int st = (h->bodyLen >= 1) ? (buf[hdr] & 0xFF) : -1;
                if (st == AUTH_PENDING) {
                    setAuthMsg(s, "等待对方在手机上点【允许】…");
                } else if (st == AUTH_DENIED) {
                    setAuthMsg(s, "已被对方拒绝");
                } else {
                    // 已批准：清掉提示（不能什么都不做，否则旧提示会永远留着）
                    setAuthMsg(s, "");
                }
                memmove(buf.data(), buf.data() + need, fill - need);
                fill -= need;
                return true;
            }

            if (h->type == PKT_STREAM_STATE) {
                if (h->bodyLen < 1) { fill = 0; return true; }
                s->remoteStreamEnabled.store(buf[hdr] != 0);
                if (buf[hdr] == 0) {
                    std::lock_guard<std::mutex> lk(mtx_);
                    if (s->jb) s->jb->reset();
                    s->fifo.clear();
                    s->fifoPos = 0;
                    s->levelPercent = 0;
                }
                memmove(buf.data(), buf.data() + need, fill - need);
                fill -= need;
                return true;
            }

            // ---- CLIPBOARD：发送端发来的文本写入 Windows 剪贴板 ----
            if (h->type == PKT_CLIPBOARD) {
                std::string text = parseClipboard(buf.data(), need);
                receiveClipboard(text);
                memmove(buf.data(), buf.data() + need, fill - need);
                fill -= need;
                return true;
            }

            // ---- AUDIO：长度随编码方式/带宽档位变化 ----
            if (h->type == PKT_AUDIO) {
                int pkt = need;
                if (h->bodyLen != sizeof(AudioHeader) +
                        (size_t)s->samples * s->channels * sizeof(int16_t)) {
                    memmove(buf.data(), buf.data() + pkt, fill - pkt);
                    fill -= pkt;
                    return true;
                }

                const AudioHeader* ah = (const AudioHeader*)(buf.data() + hdr);
                if (ah->frameSamples != s->samples) {
                    memmove(buf.data(), buf.data() + pkt, fill - pkt);
                    fill -= pkt;
                    return true;
                }
                const int16_t* pcm = (const int16_t*)(buf.data() + hdr + sizeof(AudioHeader));
                {
                    std::lock_guard<std::mutex> lk(mtx_);
                    if (s->jb) s->jb->push(ah->seq, pcm);
                }
                s->lastRecvMs.store(GetTickCount());
                s->bytes.fetch_add((uint64_t)pkt);

                // ---- 延迟估算 ----
                // 用包内源时间戳与本地单调时钟比较，并减去 30 秒窗口内的最小值，
                // 以此抵消两端时钟偏移（两端并未做时钟同步）。
                // 结果 = 抖动缓冲水位 + 网络额外延迟 + 输出缓冲，
                // 不含基础网络传输时间（那需要真正的时钟同步才能测）。
                {
                    using namespace std::chrono;
                    uint32_t myUs = (uint32_t)duration_cast<microseconds>(
                            steady_clock::now().time_since_epoch()).count();
                    int32_t d = (int32_t)(myUs - ah->timestampUs);
                    uint32_t nowMs = GetTickCount();
                    if (s->minOffsetUs == LLONG_MAX ||
                        (uint32_t)(nowMs - s->minWindowStartMs) > 30000u) {
                        s->minWindowStartMs = nowMs;
                        s->minOffsetUs = d;
                    }
                    if ((long long)d < s->minOffsetUs) s->minOffsetUs = d;
                    s->lastOffsetUs = d;

                    int pendingFrames = 0;
                    {
                        std::lock_guard<std::mutex> lk(mtx_);
                        if (s->jb) pendingFrames = s->jb->pending();
                    }
                    int netExtra = 0;
                    if (s->minOffsetUs != LLONG_MAX) {
                        long long e = (s->lastOffsetUs - s->minOffsetUs) / 1000;
                        if (e < 0) e = 0;
                        if (e > 500) e = 500;
                        netExtra = (int)e;
                    }
                    s->latencyMs.store(pendingFrames * FRAME_MS + netExtra + deviceBufferMs_);
                }

                memmove(buf.data(), buf.data() + pkt, fill - pkt);
                fill -= pkt;
                return true;
            }

            // ---- 其他类型：按 bodyLen 跳过 ----
            memmove(buf.data(), buf.data() + need, fill - need);
            fill -= need;
            return true;
        };

        // 开机先把自己的名字、编码方式与带宽档位告诉对方（对方界面就能显示本机名称）
        {
            std::string myHello = announceName_.empty() ? std::string("Windows") : announceName_;
            // 负载：[编码方式][带宽档位][名称长度][名称]
            std::vector<uint8_t> hp((size_t)sizeof(PktHeader) + 3 + myHello.size());
            PktHeader* ph = (PktHeader*)hp.data();
            memcpy(ph->magic, MAGIC, 4);
            ph->version  = PROTO_VER;
            ph->type     = PKT_HELLO;
            ph->flags    = 0;
            ph->sourceId = 0;
            ph->bodyLen  = (uint32_t)(3 + myHello.size());
            hp[sizeof(PktHeader)]     = (uint8_t)CODEC_PCM;  // 接收端不发音频，仅作声明
            hp[sizeof(PktHeader) + 1] = (uint8_t)BW_FULL;
            hp[sizeof(PktHeader) + 2] = (uint8_t)myHello.size();
            memcpy(hp.data() + sizeof(PktHeader) + 3, myHello.data(), myHello.size());
            std::lock_guard<std::mutex> lk(s->txMtx);
            send(sock, (const char*)hp.data(), (int)hp.size(), 0);
        }

        while (s->running) {
            // 至少要有通用包头才尝试解析；handleOne 返回 false 表示需要更多数据
            while (fill >= (size_t)sizeof(PktHeader) && handleOne()) { }

            int r = recv(sock, (char*)buf.data() + fill, (int)(buf.size() - fill), 0);
            if (r <= 0) break;
            fill += (size_t)r;
        }

        s->connected = false;
        s->sock.store(INVALID_SOCKET);
        s->latencyMs.store(0);
        closesocket(sock);
        if (s->running) {
            setErr(s, "连接已断开，正在重连");
            for (int i = 0; i < 10 && s->running; i++) Sleep(100);
        }
    }
}

/**
 * 每秒把本机测得的延迟回报给各发送端。
 *
 * 手机『发送』界面上会显示"延迟约 XX ms"，数据就来自这里。
 * 不做这一步，手机端会永远停在"延迟测量中…"。
 */
void Player::statsLoop() {
    while (statsRunning_.load()) {
        for (int i = 0; i < 10 && statsRunning_.load(); i++) Sleep(100);
        if (!statsRunning_.load()) break;

        std::vector<Source*> snapshot;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            snapshot = sources_;
        }

        for (auto* s : snapshot) {
            SOCKET sk = s->sock.load();
            if (sk == INVALID_SOCKET || !s->connected.load()) continue;

            uint8_t pkt[sizeof(PktHeader) + 2] = {0};
            PktHeader* h = (PktHeader*)pkt;
            memcpy(h->magic, MAGIC, 4);
            h->version  = PROTO_VER;
            h->type     = PKT_STATS;
            h->flags    = 0;
            h->sourceId = 0;
            h->bodyLen  = 2;
            int ms = s->latencyMs.load();
            if (ms < 0) ms = 0;
            if (ms > 65535) ms = 65535;
            pkt[sizeof(PktHeader)]     = (uint8_t)(ms & 0xFF);
            pkt[sizeof(PktHeader) + 1] = (uint8_t)((ms >> 8) & 0xFF);

            std::lock_guard<std::mutex> lk(s->txMtx);
            send(sk, (const char*)pkt, (int)sizeof(pkt), 0);
        }
    }
}

// ------------------------------------------------------------------
/** 设备名/格式 与 授权状态 分开存放，避免互相覆盖 */
void Player::setDevMsg(Source* s, const std::string& msg) {
    {
        std::lock_guard<std::mutex> lk(s->errMtx);
        s->devMsg = msg;
    }
    refreshErr(s);
}

/** 单独记录纯设备名，供图形界面当标题显示 */
void Player::setDevName(Source* s, const std::string& name) {
    std::lock_guard<std::mutex> lk(s->errMtx);
    s->devName = name;
}

void Player::setAuthMsg(Source* s, const std::string& msg) {
    {
        std::lock_guard<std::mutex> lk(s->errMtx);
        s->authMsg = msg;
    }
    refreshErr(s);
}

/**
 * 授权状态优先显示，其后附上设备名与格式。
 * 关键点：授权被批准时 authMsg 会被清空，提示必须随之消失 ——
 * 早期实现里 AUTH(已批准) 什么都不做，界面会一直显示
 * "等待对方在手机上点【允许】"，而音频其实已经在流。
 */
void Player::refreshErr(Source* s) {
    std::string m;
    {
        std::lock_guard<std::mutex> lk(s->errMtx);
        if (s->authMsg.empty())        m = s->devMsg;
        else if (s->devMsg.empty())    m = s->authMsg;
        else                           m = s->devMsg + "  ·  " + s->authMsg;
    }
    setErr(s, m);
}

bool Player::applySourceFormat(Source* s, int level) {
    level = bwClamp(level);
    if (s->jb && level == s->level) return false;   // 未变化
    s->level       = level;
    s->samples     = bwFrameSamples(level);
    s->channels    = bwChannels(level);
    s->packetBytes = bwPacketBytes(level);
    s->jb.reset(new JitterBuffer(s->samples * s->channels));
    s->fifo.clear();
    s->fifoPos = 0;
    s->bytes.store(0);
    return true;
}

/** 把源格式的一帧上采样成 48kHz 立体声（FRAME_SAMPLES × 2 个样本） */
void Player::upsampleTo48kStereo(const int16_t* in, int inSamples, int channels, int16_t* out) {
    const int outN = FRAME_SAMPLES;
    if (channels == 2 && inSamples == outN) {
        memcpy(out, in, (size_t)outN * CHANNELS * sizeof(int16_t));
        return;
    }
    if (inSamples <= 0) {
        memset(out, 0, (size_t)outN * CHANNELS * sizeof(int16_t));
        return;
    }
    if (channels == 1 && inSamples == outN) {
        for (int j = 0; j < outN; j++) { out[j * 2] = in[j]; out[j * 2 + 1] = in[j]; }
        return;
    }
    const double step = (double)inSamples / (double)outN;
    for (int j = 0; j < outN; j++) {
        double sp = j * step;
        int i0 = (int)sp;
        if (i0 >= inSamples) i0 = inSamples - 1;
        if (i0 < 0) i0 = 0;
        int i1 = (i0 + 1 < inSamples) ? (i0 + 1) : i0;
        double f = sp - i0;
        for (int c = 0; c < CHANNELS; c++) {
            const int srcChannel = channels == 1 ? 0 : c;
            double v = in[i0 * channels + srcChannel] +
                    (in[i1 * channels + srcChannel] - in[i0 * channels + srcChannel]) * f;
            if (v >  32767.0) v =  32767.0;
            if (v < -32768.0) v = -32768.0;
            out[j * 2 + c] = (int16_t)(v >= 0 ? (v + 0.5) : (v - 0.5));
        }
    }
}

void Player::mixInto(std::vector<int32_t>& acc, int frames, uint32_t now) {
    int16_t tmp[FRAME_SAMPLES * CHANNELS];
    int16_t up[FRAME_SAMPLES * CHANNELS];

    for (auto* s : sources_) {
        if (!s->connected.load()) { s->levelPercent.store(0); continue; }
        bool live = (now - s->lastRecvMs.load()) < 1500;
        if (!live) { s->levelPercent.store(0); continue; }
        if (!s->jb) continue;

        int have = (int)((s->fifo.size() - s->fifoPos) / CHANNELS);
        while (have < frames) {
            s->jb->pop(tmp);
            upsampleTo48kStereo(tmp, s->samples, s->channels, up);
            s->fifo.insert(s->fifo.end(), up, up + FRAME_SAMPLES * CHANNELS);
            have += FRAME_SAMPLES;
        }

        const int16_t* p = s->fifo.data() + s->fifoPos;
        const size_t cnt = (size_t)frames * CHANNELS;

        // 该路【原始】电平，供界面显示（不乘 gain，便于判断"本来就很响的是哪一路"）
        long long sumSq = 0;
        int peak = 0;
        for (size_t i = 0; i < cnt; i++) {
            int v = p[i];
            int a = (v < 0) ? -v : v;
            if (a > peak) peak = a;
            sumSq += (long long)v * (long long)v;
        }
        double rms = (cnt > 0) ? std::sqrt((double)sumSq / (double)cnt) : 0.0;
        float db = (rms > 0.0) ? (float)(20.0 * std::log10(rms / 32768.0)) : -120.0f;
        s->levelRmsDb.store(db);
        int pct = (int)(rms / 32768.0 * 100.0 * 4.0);   // 放大 4 倍便于观察
        if (pct > 100) pct = 100;
        if (pct < 0) pct = 0;
        s->levelPercent.store(pct);

        const bool  muted = s->muted.load();
        const float g     = muted ? 0.0f : s->gain.load();
        if (g != 0.0f) {
            for (size_t i = 0; i < cnt; i++) acc[i] += (int32_t)(p[i] * g);
        }

        s->fifoPos += cnt;
        if (s->fifoPos >= 8192) {
            s->fifo.erase(s->fifo.begin(), s->fifo.begin() + s->fifoPos);
            s->fifoPos = 0;
        }
    }
    // 不再做"按源数自动缩放"：那会与用户的手动调整互相打架。
    // 哪路偏大偏小由用户在界面上决定，最后只靠软限幅防削波。
}

void Player::render(int16_t* dst, int frames) {
    uint32_t now = GetTickCount();
    std::lock_guard<std::mutex> lk(mtx_);
    if (acc_.size() < (size_t)frames * CHANNELS) acc_.resize((size_t)frames * CHANNELS);
    std::fill(acc_.begin(), acc_.begin() + (size_t)frames * CHANNELS, 0);

    mixInto(acc_, frames, now);

    const float  mg  = masterGain_.load();
    const size_t cnt = (size_t)frames * CHANNELS;
    for (size_t i = 0; i < cnt; i++) {
        dst[i] = softLimit((int32_t)(acc_[i] * mg));
    }
}

Player::Source* Player::findSource(const std::string& host) {
    for (auto* s : sources_) if (s->host == host) return s;
    return nullptr;
}

bool Player::setSourceGain(const std::string& host, float gain) {
    Source* s = findSource(host);
    if (!s) return false;
    s->gain.store(clampGain(gain));
    return true;
}

bool Player::setSourceMute(const std::string& host, bool muted) {
    Source* s = findSource(host);
    if (!s) return false;
    s->muted.store(muted);
    return true;
}

std::vector<SourceStat> Player::stats() {
    uint32_t now = GetTickCount();
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<SourceStat> out;
    for (auto* s : sources_) {
        SourceStat st;
        st.host      = s->host;
        st.connected = s->connected.load() && s->remoteStreamEnabled.load();
        st.recentAudio = s->lastRecvMs.load() != 0
                && (now - s->lastRecvMs.load()) < 1500;
        st.level     = s->level;
        {
            std::lock_guard<std::mutex> lk(s->errMtx);
            st.name = s->devName;
            if (s->authMsg.find("允许") != std::string::npos) st.pendingApproval = true;
            if (s->authMsg.find("拒绝") != std::string::npos) st.denied = true;
        }
        if (s->jb) {
            st.pending   = s->jb->pending();
            st.warmedUp  = s->jb->warmedUp();
            st.recv      = s->jb->recvFrames;
            st.lost      = s->jb->lostFrames;
            st.dup       = s->jb->dupFrames;
            st.dropped   = s->jb->droppedFrames;
            st.late      = s->jb->lateFrames;
            st.underruns = s->jb->underruns;
        } else {
            st.pending = 0; st.warmedUp = false;
            st.recv = st.lost = st.dup = st.dropped = st.late = st.underruns = 0;
        }
        st.pendingMs = st.pending * FRAME_MS;
        st.bytes     = s->bytes.load();
        st.gain       = s->gain.load();
        st.muted      = s->muted.load();
        st.levelRmsDb = s->levelRmsDb.load();
        st.levelPercent = s->levelPercent.load();
        {
            std::lock_guard<std::mutex> lk2(s->errMtx);
            st.err = s->err;
        }
        (void)now;
        out.push_back(st);
    }
    return out;
}

} // namespace ahub
