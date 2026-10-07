#pragma once

#include "common.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>

namespace ahub {

/** 电脑发送端：在同一 TCP 端口上提供 HELLO 与双向剪贴板消息。 */
class ClipboardServer {
public:
    using ClipboardHandler = std::function<void(const std::string&)>;
    struct PeerStat {
        std::string name;
        std::string address;
        int latencyMs = -1;
        bool audioEnabled = true;
    };

    ~ClipboardServer();
    bool start(const std::string& bindIp, uint16_t port, const std::string& name,
               ClipboardHandler handler, std::string& err);
    void stop();
    int peerCount() const;
    std::vector<std::string> peerAddresses() const;
    std::vector<PeerStat> peerStats() const;
    bool setPeerAudioEnabled(const std::string& address, bool enabled);
    bool isListening() const { return running_.load(); }
    bool requestScan();
    bool isScanning() const { return scanning_.load(); }
    int send(const std::string& text);
    int sendAudio(const int16_t* stereo, int frames);

private:
    struct Peer {
        std::atomic<SOCKET> sock{INVALID_SOCKET};
        std::atomic<bool> alive{true};
        std::atomic<bool> hasHello{false};
        std::atomic<bool> audioEnabled{true};
        std::atomic<int> latencyMs{-1};
        std::string name;
        std::string address;
        std::mutex writeMtx;
        std::thread thread;
    };

    void acceptLoop();
    void connectLoop();
    void attachSocket(SOCKET s, const std::string& address);
    bool hasAlivePeer(const std::string& address) const;
    void peerLoop(const std::shared_ptr<Peer>& peer);
    bool sendPacket(const std::shared_ptr<Peer>& peer, const std::vector<uint8_t>& pkt);
    std::vector<uint8_t> helloPacket() const;

    SOCKET listenSock_ = INVALID_SOCKET;
    std::thread acceptThread_;
    std::thread connectThread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> scanning_{false};
    std::mutex scanMtx_;
    std::condition_variable scanCv_;
    bool scanRequested_ = false;
    std::string name_ = "Windows（电脑）";
    std::string bindIp_;
    ClipboardHandler handler_;
    mutable std::mutex peersMtx_;
    std::vector<std::shared_ptr<Peer>> peers_;
    std::vector<std::string> knownAddresses_; // 本次服务中真正握手成功的设备
    std::atomic<uint32_t> audioSeq_{0};
};

} // namespace ahub
