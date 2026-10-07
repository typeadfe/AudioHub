/*
 * 播放端核心：多源 TCP 连接 + 抖动缓冲 + 混音台
 *
 * 【设计要点 —— 为什么是 TCP 且由播放端主动连】
 * 早期实现让播放端监听 UDP，撞上 Windows 防火墙入站拦截，放行需管理员权限。
 * 任何 Windows/macOS/Linux 默认都有这个限制，"让接收端监听"是错的设计。
 * 改为：手机做 TCP 服务端，播放端主动连过去 —— 出站连接不受任何防火墙阻挡。
 *
 * 【混音台设计】
 *   - 每路源有独立的 gain(0~2) 与 mute，由用户在浏览器控制界面调整
 *   - 另有一路 master gain
 *   - 不再使用"按源数自动缩放"：那会与用户的手动调整互相打架，
 *     实际使用中"哪路偏大偏小"必须由人决定，机器猜不准
 *   - 最后一道是软限幅，只防削波，不影响正常音量
 */
#pragma once

#include "common.h"
#include "clipboard_server.h"
#include "jitter_buffer.h"
#include "loopback_sender.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ahub {

/** 一个源的对外统计 */
struct SourceStat {
    std::string host;
    /** 设备名（由 HELLO 得到；未握手时为空，界面回退显示 host） */
    std::string name;
    bool        connected = false;
    int         pending    = 0;      // 抖动缓冲待播帧数
    int         pendingMs  = 0;
    bool        warmedUp   = false;
    bool        recentAudio = false;
    uint64_t    recv = 0, lost = 0, dup = 0, dropped = 0, late = 0, underruns = 0;
    uint64_t    bytes = 0;
    std::string err;
    // 混音台
    float       gain  = 1.0f;
    bool        muted = false;
    float       levelRmsDb = -120.0f;   // 该源原始电平（未乘 gain）
    int         levelPercent = 0;       // 0~100，供界面画电平条
    int         level = BW_FULL;        // 源端带宽档位
    // 安全 / 兼容状态：界面据此给出准确提示，而不是含糊的"已连接"
    bool        pendingApproval = false;  // 等待发送端用户点【允许】
    bool        denied          = false;  // 已被发送端拒绝
};

class Player {
public:
    ~Player();
    Player() = default;
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    bool init(const std::vector<std::string>& hosts, uint16_t port, std::string& err);
    bool startSenderService(std::string& err);
    void stopSenderService();
    bool startReceiverService(std::string& err);
    void stopReceiverService();
    bool receiverRunning() const { return announceRunning_.load(); }
    /** 运行中动态新增一个源；已存在则返回 false */
    bool addHost(const std::string& host, uint16_t port);
    void shutdown();

    /** 由 WASAPI 回调调用：产生 frames 帧交错立体声 int16 */
    void render(int16_t* dst, int frames);

    std::vector<SourceStat> stats();
    int connectedSources();
    bool hasRecentAudio();

    /** 发送当前 Windows 剪贴板到所有已连接设备。 */
    int sendClipboard();
    /** 最近收到的剪贴板文本；收到后也会写入 Windows 系统剪贴板。 */
    std::string clipboardText() const;
    uint64_t clipboardReceiveSeq() const { return clipboardReceiveSeq_.load(); }
    bool copyReceivedClipboard() const;
    int clipboardPeerCount() const { return clipboardServer_.peerCount(); }
    std::vector<std::string> clipboardPeerAddresses() const { return clipboardServer_.peerAddresses(); }
    std::vector<ClipboardServer::PeerStat> senderPeers() const { return clipboardServer_.peerStats(); }
    bool setSenderPeerEnabled(const std::string& address, bool enabled) {
        return clipboardServer_.setPeerAudioEnabled(address, enabled);
    }
    bool senderListening() const { return clipboardServer_.isListening(); }
    bool requestSenderScan() { return clipboardServer_.requestScan(); }
    bool senderScanning() const { return clipboardServer_.isScanning(); }
    std::string senderListenError() const { return senderListenError_; }
    bool startAudioSender(std::string& err);
    void stopAudioSender() { loopbackSender_.stop(); }
    bool isAudioSenderRunning() const { return loopbackSender_.isRunning(); }

    // ---- 混音台控制 ----
    /** 按 host 设置某路增益，范围会被夹到 0~2；返回是否找到该源 */
    bool setSourceGain(const std::string& host, float gain);
    bool setSourceMute(const std::string& host, bool muted);
    void setMasterGain(float g) { masterGain_.store(clampGain(g)); }
    float masterGain() const { return masterGain_.load(); }

    /** 本机所有可用的 IPv4 地址，结果按 WiFi/有线局域网优先排序 */
    static std::vector<std::string> localIPv4();
    /** 本机所在网段的候选地址列表（.1 ~ .254） */
    std::vector<std::string> localSubnetHosts() const;
    /** 扫描这些地址的指定端口，返回接受连接的主机 */
    std::vector<std::string> scan(uint16_t port, int timeoutMs, int* scanned);

    /**
     * 与 scan 相同，但【跳过已经建立连接的地址】。
     *
     * 扫描是"连一下再关掉"的探测，手机端在 TCP 层无法把它和真实接收端区分开。
     * 不跳过已连上的地址，每轮扫描都会打断一次正在收音频的连接，
     * 表现为声音一卡一卡、设备反复掉线。
     */
    std::vector<std::string> scanNew(uint16_t port, int timeoutMs, int* scanned,
                                     const std::atomic<bool>* keepRunning = nullptr);

    /** 设置广播用的设备名（手机『发送』界面上显示的名字） */
    void setAnnounceName(const std::string& n) { announceName_ = n; }

    /**
     * 选择本机用于局域网音频的地址。留空时自动优先 WiFi，随后有线网，
     * 最后才考虑 VPN/TUN 地址。传入不属于本机的地址会被忽略。
     */
    void setPreferredIp(const std::string& ip);
    /** 当前实际使用的局域网地址 */
    std::string preferredIp() const { return preferredIp_; }

    /** WASAPI 输出缓冲毫秒数（用于延迟估算） */
    void setDeviceBufferMs(int ms) { if (ms > 0 && ms < 500) deviceBufferMs_ = ms; }

private:
    struct Source {
        std::string host;
        uint16_t    port = 0;
        std::thread th;
        /** 当前连接（供延迟上报线程发送用；未连接时为 INVALID_SOCKET） */
        std::atomic<SOCKET> sock{INVALID_SOCKET};
        std::atomic<bool>  running{false};
        std::atomic<bool>  connected{false};
        std::atomic<bool>  remoteStreamEnabled{true};
        std::atomic<float> gain{1.0f};
        std::atomic<bool>  muted{false};
        std::atomic<float> levelRmsDb{-120.0f};
        std::atomic<int>   levelPercent{0};

        // ---- 源端格式（由 HELLO 决定，音频包长度随之变化）----
        int level       = BW_FULL;
        int quality     = 1;
        int samples     = FRAME_SAMPLES;      // 每声道样本数
        int channels    = CHANNELS;
        int packetBytes = AUDIO_PKT_BYTES;
        std::unique_ptr<JitterBuffer> jb;     // 收到 HELLO 后才创建

        // ---- 延迟估算（用于向发送端回报延迟）----
        long long minOffsetUs      = LLONG_MAX;
        uint32_t  minWindowStartMs = 0;
        long long lastOffsetUs     = 0;
        std::atomic<int> latencyMs{0};

        // ---- 状态文案 ----
        // 设备名、设备名+格式 的展示串、授权状态 三者分开保存：
        // 早期把它们挤在一个字段里，导致授权被批准后提示无法清除，
        // 界面会一直显示"等待对方点允许"，而音频其实已经在流。
        std::string devName;    // 由 HELLO 得到的纯设备名（界面标题用）
        std::string devMsg;     // "名字（格式，码率）" 展示串
        std::string authMsg;    // 授权相关提示

        std::vector<int16_t> fifo;            // 已上采样为 48kHz 立体声
        size_t            fifoPos = 0;
        std::atomic<uint32_t> lastRecvMs{0};
        std::atomic<uint64_t> bytes{0};
        std::mutex txMtx;
        std::mutex        errMtx;
        std::string       err;
    };

    static float clampGain(float g) {
        if (g < 0.0f) return 0.0f;
        if (g > 2.0f) return 2.0f;
        return g;
    }

    void sourceLoop(Source* s);
    /** 收到 HELLO 后应用源端带宽格式；返回是否发生了变化 */
    bool applySourceFormat(Source* s, int level);
    /**
     * 周期性 UDP 广播本机存在。
     *
     * 连接方向是"接收端主动连发送端"（为绕开 Windows 防火墙入站拦截），
     * 所以发送端（手机）无法主动发现本播放端 —— 不做这个广播的话，
     * 手机的『发送』界面上永远看不到电脑。UDP 是出站，不受入站规则影响。
     */
    void announceLoop();
    std::thread       announceThread_;
    std::atomic<bool> announceRunning_{false};
    std::string       announceName_ = "Windows";
    std::string       preferredIp_;

    /** 每秒把本机测得的延迟回报给各发送端（手机界面上就能看到延迟） */
    void statsLoop();
    std::thread       statsThread_;
    std::atomic<bool> statsRunning_{false};

    /** 供状态文案使用：优先显示授权状态，其次显示设备名与格式 */
    void setDevMsg(Source* s, const std::string& msg);
    /** 只记录纯设备名（图形界面当标题用），不参与状态文案拼接 */
    void setDevName(Source* s, const std::string& name);
    void setAuthMsg(Source* s, const std::string& msg);
    void refreshErr(Source* s);
    /** 把源格式的一帧上采样成 48kHz 立体声 */
    static void upsampleTo48kStereo(const int16_t* in, int inSamples, int channels, int16_t* out);
    void mixInto(std::vector<int32_t>& acc, int frames, uint32_t now);
    void setErr(Source* s, const std::string& msg);
    Source* findSource(const std::string& host);
    void receiveClipboard(const std::string& text);

    std::vector<Source*> sources_;
    std::vector<int32_t> acc_;
    std::atomic<float>   masterGain_{1.0f};
    std::mutex mtx_;
    ClipboardServer clipboardServer_;
    std::string senderListenError_;
    LoopbackSender loopbackSender_;
    mutable std::mutex clipboardMtx_;
    std::string clipboardText_;
    std::atomic<uint64_t> clipboardReceiveSeq_{0};
    /** 输出缓冲毫秒数。估算延迟用，实际值由 main 从 WASAPI 取得 */
    int deviceBufferMs_ = 30;
};

} // namespace ahub
