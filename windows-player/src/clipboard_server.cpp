#include "clipboard_server.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace ahub {

ClipboardServer::~ClipboardServer() { stop(); }

static std::vector<uint8_t> makeHello(const std::string& name) {
    std::string nm = name.empty() ? "Windows（电脑）" : name;
    if (nm.size() > 60) nm.resize(60);
    std::vector<uint8_t> pkt(sizeof(PktHeader) + 3 + nm.size());
    PktHeader* h = (PktHeader*)pkt.data();
    memcpy(h->magic, MAGIC, 4);
    h->version = PROTO_VER;
    h->type = PKT_HELLO;
    h->flags = 0;
    h->sourceId = 0;
    h->bodyLen = (uint32_t)(3 + nm.size());
    pkt[sizeof(PktHeader)] = (uint8_t)CODEC_PCM;
    pkt[sizeof(PktHeader) + 1] = (uint8_t)BW_FULL;
    pkt[sizeof(PktHeader) + 2] = (uint8_t)nm.size();
    if (!nm.empty()) memcpy(pkt.data() + sizeof(PktHeader) + 3, nm.data(), nm.size());
    return pkt;
}

static std::vector<uint8_t> makeStreamState(bool enabled) {
    std::vector<uint8_t> pkt(sizeof(PktHeader) + 1);
    PktHeader* h = (PktHeader*)pkt.data();
    memcpy(h->magic, MAGIC, 4);
    h->version = PROTO_VER;
    h->type = PKT_STREAM_STATE;
    h->flags = 0;
    h->sourceId = 0;
    h->bodyLen = 1;
    pkt[sizeof(PktHeader)] = enabled ? 1 : 0;
    return pkt;
}

bool ClipboardServer::start(const std::string& bindIp, uint16_t port,
                            const std::string& name, ClipboardHandler handler,
                            std::string& err) {
    if (running_) return true;
    name_ = name.empty() ? "Windows（电脑）" : name;
    bindIp_ = bindIp;
    handler_ = std::move(handler);

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { err = "剪贴板服务创建 socket 失败"; return false; }
    BOOL yes = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&yes, sizeof(yes));

    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = bindIp.empty() ? INADDR_ANY : inet_addr(bindIp.c_str());
    if (a.sin_addr.s_addr == INADDR_NONE && !bindIp.empty()) {
        closesocket(s); err = "剪贴板服务绑定地址无效"; return false;
    }
    if (bind(s, (sockaddr*)&a, sizeof(a)) == SOCKET_ERROR ||
            listen(s, 8) == SOCKET_ERROR) {
        // Windows 入站监听可以被防火墙或旧实例占用；手机反向入口仍可用。
        closesocket(s);
        s = INVALID_SOCKET;
    }
    listenSock_ = s;
    running_ = true;
    scanning_ = false;
    scanRequested_ = false;
    if (s != INVALID_SOCKET) acceptThread_ = std::thread(&ClipboardServer::acceptLoop, this);
    connectThread_ = std::thread(&ClipboardServer::connectLoop, this);
    return true;
}

void ClipboardServer::stop() {
    running_ = false;
    scanCv_.notify_all();
    SOCKET ls = listenSock_;
    listenSock_ = INVALID_SOCKET;
    if (ls != INVALID_SOCKET) {
        shutdown(ls, SD_BOTH);
        closesocket(ls);
    }
    if (acceptThread_.joinable()) acceptThread_.join();
    if (connectThread_.joinable()) connectThread_.join();
    scanning_ = false;

    std::vector<std::shared_ptr<Peer>> peers;
    {
        std::lock_guard<std::mutex> lk(peersMtx_);
        peers = peers_;
    }
    for (const auto& p : peers) {
        p->alive = false;
        SOCKET s = p->sock.exchange(INVALID_SOCKET);
        if (s != INVALID_SOCKET) {
            shutdown(s, SD_BOTH);
            closesocket(s);
        }
    }
    for (const auto& p : peers) if (p->thread.joinable()) p->thread.join();
    {
        std::lock_guard<std::mutex> lk(peersMtx_);
        peers_.clear();
        knownAddresses_.clear();
    }
}

bool ClipboardServer::requestScan() {
    if (!running_ || bindIp_.empty() || scanning_) return false;
    {
        std::lock_guard<std::mutex> lk(scanMtx_);
        if (scanRequested_) return false;
        scanRequested_ = true;
    }
    scanCv_.notify_one();
    return true;
}

std::vector<uint8_t> ClipboardServer::helloPacket() const { return makeHello(name_); }

void ClipboardServer::acceptLoop() {
    while (running_) {
        sockaddr_in from{};
        int len = sizeof(from);
        SOCKET s = accept(listenSock_, (sockaddr*)&from, &len);
        if (s == INVALID_SOCKET) {
            if (running_) Sleep(20);
            continue;
        }
        // 扫描探测会频繁建立短连接，及时回收其线程与记录。
        {
            std::vector<std::shared_ptr<Peer>> finished;
            {
                std::lock_guard<std::mutex> lk(peersMtx_);
                auto it = peers_.begin();
                while (it != peers_.end()) {
                    if (!(*it)->alive) { finished.push_back(*it); it = peers_.erase(it); }
                    else ++it;
                }
            }
            for (const auto& p : finished) if (p->thread.joinable()) p->thread.join();
        }
        attachSocket(s, inet_ntoa(from.sin_addr));
    }
}

bool ClipboardServer::hasAlivePeer(const std::string& address) const {
    std::lock_guard<std::mutex> lk(peersMtx_);
    for (const auto& p : peers_) if (p->alive && p->address == address) return true;
    return false;
}

void ClipboardServer::attachSocket(SOCKET s, const std::string& address) {
    if (!running_) { closesocket(s); return; }
    BOOL yes = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&yes, sizeof(yes));
    int timeout = 5000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    timeout = 1000;
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));
    auto peer = std::make_shared<Peer>();
    peer->sock = s;
    peer->address = address;
    {
        std::lock_guard<std::mutex> lk(peersMtx_);
        for (const auto& p : peers_) {
            if (p->alive && p->address == address) { closesocket(s); return; }
        }
        peers_.push_back(peer);
    }
    if (!sendPacket(peer, helloPacket())) {
        peer->alive = false;
        closesocket(peer->sock.exchange(INVALID_SOCKET));
        return;
    }
    peer->thread = std::thread(&ClipboardServer::peerLoop, this, peer);
}

void ClipboardServer::connectLoop() {
    const auto dot = bindIp_.rfind('.');
    if (dot == std::string::npos) return;
    const std::string prefix = bindIp_.substr(0, dot + 1);
    while (running_) {
        bool manual = false;
        {
            std::unique_lock<std::mutex> lk(scanMtx_);
            scanCv_.wait_for(lk, std::chrono::seconds(15),
                             [&]() { return !running_ || scanRequested_; });
            if (!running_) break;
            manual = scanRequested_;
            scanRequested_ = false;
            scanning_ = manual;
        }
        std::vector<std::string> targets;
        if (manual) {
            for (int octet = 1; octet < 255; ++octet)
                targets.push_back(prefix + std::to_string(octet));
        } else {
            std::lock_guard<std::mutex> lk(peersMtx_);
            targets = knownAddresses_;
        }
        const uint32_t deadline = GetTickCount() + 5000u;
        std::atomic<size_t> next{0};
        std::vector<std::thread> workers;
        const size_t workerCount = std::min(targets.size(), manual ? size_t(16) : size_t(2));
        for (size_t t = 0; t < workerCount; ++t) {
            workers.emplace_back([&, this]() {
                while (running_) {
                    if ((int32_t)(GetTickCount() - deadline) >= 0) break;
                    size_t i = next.fetch_add(1);
                    if (i >= targets.size()) break;
                    const std::string& host = targets[i];
                    if (host == bindIp_ || hasAlivePeer(host)) continue;
                    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                    if (s == INVALID_SOCKET) continue;
                    if (!bindIp_.empty()) {
                        sockaddr_in local{};
                        local.sin_family = AF_INET;
                        local.sin_addr.s_addr = inet_addr(bindIp_.c_str());
                        local.sin_port = 0;
                        if (bind(s, (sockaddr*)&local, sizeof(local)) == SOCKET_ERROR) {
                            closesocket(s); continue;
                        }
                    }
                    u_long nonblocking = 1;
                    ioctlsocket(s, FIONBIO, &nonblocking);
                    sockaddr_in remote{};
                    remote.sin_family = AF_INET;
                    remote.sin_port = htons(RECEIVER_PORT);
                    remote.sin_addr.s_addr = inet_addr(host.c_str());
                    int rc = connect(s, (sockaddr*)&remote, sizeof(remote));
                    if (rc == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
                        closesocket(s); continue;
                    }
                    fd_set writable;
                    FD_ZERO(&writable);
                    FD_SET(s, &writable);
                    timeval tv{0, 220000};
                    rc = select(0, nullptr, &writable, nullptr, &tv);
                    int sockError = 0;
                    int len = sizeof(sockError);
                    if (rc <= 0 || getsockopt(s, SOL_SOCKET, SO_ERROR,
                            (char*)&sockError, &len) == SOCKET_ERROR || sockError != 0) {
                        closesocket(s); continue;
                    }
                    nonblocking = 0;
                    ioctlsocket(s, FIONBIO, &nonblocking);
                    attachSocket(s, host);
                }
            });
        }
        for (auto& worker : workers) worker.join();
        scanning_ = false;
        std::vector<std::shared_ptr<Peer>> finished;
        {
            std::lock_guard<std::mutex> lk(peersMtx_);
            auto it = peers_.begin();
            while (it != peers_.end()) {
                if (!(*it)->alive) { finished.push_back(*it); it = peers_.erase(it); }
                else ++it;
            }
        }
        for (const auto& peer : finished) if (peer->thread.joinable()) peer->thread.join();
    }
}

bool ClipboardServer::sendPacket(const std::shared_ptr<Peer>& peer,
                                  const std::vector<uint8_t>& pkt) {
    if (!peer || !peer->alive || pkt.empty()) return false;
    std::lock_guard<std::mutex> lk(peer->writeMtx);
    SOCKET s = peer->sock.load();
    if (s == INVALID_SOCKET) return false;
    size_t off = 0;
    while (off < pkt.size()) {
        int n = ::send(s, (const char*)pkt.data() + off, (int)(pkt.size() - off), 0);
        if (n <= 0) { peer->alive = false; return false; }
        off += (size_t)n;
    }
    return true;
}

void ClipboardServer::peerLoop(const std::shared_ptr<Peer>& peer) {
    std::vector<uint8_t> buf(CLIPBOARD_MAX_BYTES + sizeof(PktHeader) + 32);
    size_t fill = 0;
    const auto closePeer = [&]() {
        peer->alive = false;
        SOCKET s = peer->sock.exchange(INVALID_SOCKET);
        if (s != INVALID_SOCKET) closesocket(s);
    };
    while (running_ && peer->alive) {
        SOCKET s = peer->sock.load();
        if (s == INVALID_SOCKET) break;
        int n = recv(s, (char*)buf.data() + fill, (int)(buf.size() - fill), 0);
        if (n <= 0) break;
        fill += (size_t)n;
        while (fill >= sizeof(PktHeader)) {
            if (!validPacketHeader(buf.data(), (int)fill)) { fill = 0; break; }
            const PktHeader* h = (const PktHeader*)buf.data();
            size_t need = sizeof(PktHeader) + (size_t)h->bodyLen;
            if (need > buf.size()) { fill = 0; break; }
            if (fill < need) break;
            if (h->type == PKT_HELLO) {
                {
                    std::lock_guard<std::mutex> lk(peersMtx_);
                    peer->name = helloName(buf.data(), (int)need);
                    if (std::find(knownAddresses_.begin(), knownAddresses_.end(), peer->address)
                            == knownAddresses_.end()) knownAddresses_.push_back(peer->address);
                }
                peer->hasHello = true;
                sendPacket(peer, makeStreamState(peer->audioEnabled));
            } else if (h->type == PKT_STATS && h->bodyLen >= 2 && peer->hasHello) {
                int ms = buf[sizeof(PktHeader)]
                       | ((int)buf[sizeof(PktHeader) + 1] << 8);
                peer->latencyMs = ms;
            } else if (h->type == PKT_CLIPBOARD && peer->hasHello && handler_) {
                handler_(parseClipboard(buf.data(), (int)need));
            }
            memmove(buf.data(), buf.data() + need, fill - need);
            fill -= need;
        }
    }
    closePeer();
}

int ClipboardServer::peerCount() const {
    int n = 0;
    std::lock_guard<std::mutex> lk(peersMtx_);
    for (const auto& p : peers_) if (p->alive && p->hasHello) n++;
    return n;
}

std::vector<std::string> ClipboardServer::peerAddresses() const {
    std::vector<std::string> out;
    std::lock_guard<std::mutex> lk(peersMtx_);
    for (const auto& p : peers_)
        if (p->alive && p->hasHello) out.push_back(p->address);
    return out;
}

std::vector<ClipboardServer::PeerStat> ClipboardServer::peerStats() const {
    std::vector<PeerStat> out;
    std::lock_guard<std::mutex> lk(peersMtx_);
    for (const auto& p : peers_) {
        if (!p->alive || !p->hasHello) continue;
        PeerStat s;
        s.name = p->name.empty() ? p->address : p->name;
        s.address = p->address;
        s.latencyMs = p->latencyMs;
        s.audioEnabled = p->audioEnabled;
        out.push_back(s);
    }
    std::sort(out.begin(), out.end(), [](const PeerStat& a, const PeerStat& b) {
        return a.name < b.name;
    });
    return out;
}

bool ClipboardServer::setPeerAudioEnabled(const std::string& address, bool enabled) {
    std::lock_guard<std::mutex> lk(peersMtx_);
    bool found = false;
    for (const auto& p : peers_) {
        if (!p->alive || !p->hasHello || p->address != address) continue;
        p->audioEnabled = enabled;
        sendPacket(p, makeStreamState(enabled));
        found = true;
    }
    return found;
}

int ClipboardServer::send(const std::string& text) {
    const std::vector<uint8_t> pkt = buildClipboard(text);
    int n = 0;
    std::lock_guard<std::mutex> lk(peersMtx_);
    for (const auto& p : peers_) {
        if (p->alive && p->hasHello && sendPacket(p, pkt)) n++;
    }
    return n;
}

int ClipboardServer::sendAudio(const int16_t* stereo, int frames) {
    if (!stereo || frames != FRAME_SAMPLES) return 0;
    std::vector<uint8_t> pkt(sizeof(PktHeader) + sizeof(AudioHeader) + FRAME_BYTES);
    PktHeader* h = (PktHeader*)pkt.data();
    memcpy(h->magic, MAGIC, 4);
    h->version = PROTO_VER;
    h->type = PKT_AUDIO;
    h->flags = 0;
    h->sourceId = 0;
    h->bodyLen = (uint32_t)(sizeof(AudioHeader) + FRAME_BYTES);
    AudioHeader* ah = (AudioHeader*)(pkt.data() + sizeof(PktHeader));
    ah->seq = (uint16_t)audioSeq_.fetch_add(1);
    ah->frameSamples = FRAME_SAMPLES;
    using namespace std::chrono;
    ah->timestampUs = (uint32_t)duration_cast<microseconds>(
            steady_clock::now().time_since_epoch()).count();
    memcpy(pkt.data() + sizeof(PktHeader) + sizeof(AudioHeader), stereo, FRAME_BYTES);
    int n = 0;
    std::lock_guard<std::mutex> lk(peersMtx_);
    for (const auto& p : peers_) {
        if (p->alive && p->hasHello && p->audioEnabled && sendPacket(p, pkt)) n++;
    }
    return n;
}

} // namespace ahub
