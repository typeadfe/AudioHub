/*
 * AudioHub 公共定义：音频格式 与 传输协议
 *
 * 当前 WiFi 链路使用 TCP 字节流传输 PCM16，音频帧有通用包头和可变长度负载。
 * 档位定义须与 Android Protocol.java 保持一致。
 */
#pragma once

#include <cstdint>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

// 本协议的包头按【小端序 little-endian】定义。
// 目标平台 x86_64(Windows) 与 ARM64(Android) 均为小端，因此结构体可直接映射，
// 不需要逐字段做字节序转换。若将来要移植到大端平台，必须改为显式转换。
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__)
#error "AudioHub 协议按小端序定义，当前平台是大端，需要显式字节序转换"
#endif

// 所有多字节字段都依赖编译器不做插入填充，必须 1 字节对齐
#pragma pack(push, 1)

namespace ahub {

// ---------------- 音频格式 ----------------
constexpr int SAMPLE_RATE   = 48000;
constexpr int CHANNELS      = 2;
constexpr int BITS          = 16;
constexpr int BYTES_PER_SMP = BITS / 8;

/** 帧长 5ms */
constexpr int FRAME_MS      = 5;
/** 每帧每声道的采样数：48000 * 0.005 = 240 */
constexpr int FRAME_SAMPLES = SAMPLE_RATE * FRAME_MS / 1000;   // 240
/** 每帧总字节（交错立体声）：240 * 2 * 2 = 960 */
constexpr int FRAME_BYTES   = FRAME_SAMPLES * CHANNELS * BYTES_PER_SMP;  // 960

/** 每秒帧数：200 */
constexpr int FRAMES_PER_SEC = 1000 / FRAME_MS;

// ---------------- 协议 ----------------
constexpr char     MAGIC[4]     = {'A', 'H', 'U', 'B'};
constexpr uint8_t  PROTO_VER    = 1;

/** 手机发送端监听的 TCP 端口 */
constexpr uint16_t DISCOVERY_PORT = 53535;
/** 手机接收端监听，电脑发送端主动连入。 */
constexpr uint16_t RECEIVER_PORT = 53536;

enum PacketType : uint8_t {
    PKT_DISCOVER = 0x01,   // 源 -> 广播
    PKT_REPLY    = 0x02,   // 播放端 -> 源
    PKT_BYE      = 0x03,   // 源 -> 广播
    PKT_HELLO    = 0x04,   // 握手：设备名 + 带宽档位（双向，连接后各发一次）
    PKT_STATS    = 0x05,   // 接收端 -> 发送端：延迟上报（2 字节毫秒）
    PKT_AUTH     = 0x06,   // 发送端 -> 接收端：授权状态（1 字节，见 AUTH_*）
    PKT_ANNOUNCE = 0x20,   // 接收端 -> 局域网：UDP 存在广播（负载为设备名）
    PKT_AUDIO    = 0x10,   // 源 -> 播放端
    PKT_CLIPBOARD = 0x30,  // 双向：UTF-8 剪贴板文本
    PKT_STREAM_STATE = 0x31, // 双向：该连接是否发送声音；剪贴板不受影响
};

/** 授权状态取值，与 Android 端 Protocol.AUTH_* 必须一致 */
constexpr int AUTH_PENDING  = 0;   // 等待发送端用户批准
constexpr int AUTH_APPROVED = 1;   // 已批准，开始发送
constexpr int AUTH_DENIED   = 2;   // 已被拒绝
constexpr size_t CLIPBOARD_MAX_BYTES = 64u * 1024u;

/** 接收端存在广播用的 UDP 端口 */
constexpr uint16_t ANNOUNCE_PORT = 53537;

/** 通用包头，16 字节 */
struct PktHeader {
    char     magic[4];     // "AHUB"
    uint8_t  version;      // PROTO_VER
    uint8_t  type;         // PacketType
    uint16_t flags;        // 保留
    uint32_t sourceId;     // 源实例随机 ID
    uint32_t bodyLen;      // 负载长度（不含通用包头）
};

/** 音频包头，8 字节，紧跟在 PktHeader 之后 */
struct AudioHeader {
    uint16_t seq;          // 序号，用于丢包检测与重排
    uint16_t frameSamples; // 本帧每声道采样数（应为 FRAME_SAMPLES）
    uint32_t timestampUs;  // 源端单调时钟微秒（v1 仅用于诊断）
};

/** 控制包负载（DISCOVER / REPLY 共用） */
struct CtrlBody {
    uint16_t dataPort;     // 发送方用于收发音频的 UDP 端口
    uint8_t  nameLen;      // 设备名长度
    // char name[nameLen];  UTF-8，紧随其后
};

#pragma pack(pop)

/** 音频包总头长 */
constexpr int AUDIO_PKT_HEADER = sizeof(PktHeader) + sizeof(AudioHeader);  // 24
/** 一个完整音频数据报的字节数（无损档位下的值；实际随带宽档位变化） */
constexpr int AUDIO_PKT_BYTES  = AUDIO_PKT_HEADER + FRAME_BYTES;           // 984

// ================== 编码方式 ==================
//
// 与 Android 端 Protocol.CODEC_PCM 保持一致。
constexpr int CODEC_PCM  = 0;

static_assert(sizeof(PktHeader) == 16, "PktHeader 应为 16 字节");
static_assert(sizeof(AudioHeader) == 8, "AudioHeader 应为 8 字节");
static_assert(AUDIO_PKT_BYTES < 1500, "音频包必须小于 MTU，避免 IP 分片");

/** 抖动缓冲目标水位（毫秒） */
constexpr int JITTER_TARGET_MS = 40;
/** 抖动缓冲水位上下限（毫秒），超出即纠偏 */
constexpr int JITTER_HIGH_MS   = 80;
constexpr int JITTER_LOW_MS    = 20;

// ================== 带宽档位 ==================
//
// 用【采样率 + 声道】实现，不引入编解码器。
// 与 Android 端 Protocol.java 中的定义必须保持一致。
//
// 档位            采样率   声道    有效载荷带宽
//   BW_FULL       48000    2       1536 kbps
//   BW_HIGH       24000    2        768 kbps
//   BW_MED        16000    2        512 kbps
//   BW_LOW        16000    1        256 kbps
//
// 8kHz 那一档已去掉：音频带宽只有 4kHz，听音乐和游戏完全不可用。
constexpr int BW_FULL  = 0;
constexpr int BW_HIGH  = 1;
constexpr int BW_MED   = 2;
constexpr int BW_LOW   = 3;
constexpr int BW_COUNT = 4;

inline int bwClamp(int level) {
    if (level < 0) return 0;
    if (level >= BW_COUNT) return BW_COUNT - 1;
    return level;
}

inline int bwSampleRate(int level) {
    switch (bwClamp(level)) {
        case BW_HIGH: return 24000;
        case BW_MED: return 16000;
        case BW_LOW: return 16000;
        default:     return 48000;
    }
}

inline int bwChannels(int level) { return (bwClamp(level) == BW_LOW) ? 1 : 2; }

inline int bwFrameSamples(int level) {
    return bwSampleRate(level) * FRAME_MS / 1000;
}

inline int bwFrameBytes(int level) {
    return bwFrameSamples(level) * bwChannels(level) * BYTES_PER_SMP;
}

inline int bwPacketBytes(int level) {
    return AUDIO_PKT_HEADER + bwFrameBytes(level);
}

inline int bwKbps(int level) {
    return bwSampleRate(level) * bwChannels(level) * 16 / 1000;
}

inline const char* bwLabel(int level) {
    switch (bwClamp(level)) {
        case BW_HIGH: return "高保真";
        case BW_MED:  return "标准";
        case BW_LOW:  return "省流";
        default:      return "无损原音";
    }
}

// ------------------------------------------------------------------
// HELLO 负载格式：[编码方式 1 字节][带宽档位 1 字节][名称长度 1 字节][UTF-8 名称]
// 注意：以下函数必须放在 CODEC_* 与 BW_* 定义【之后】。
// ------------------------------------------------------------------

/** 从 HELLO 取带宽档位 */
inline int helloLevel(const uint8_t* pkt, int len) {
    if (len < (int)sizeof(PktHeader) + 2) return BW_FULL;
    return bwClamp(pkt[sizeof(PktHeader) + 1] & 0xFF);
}

/** 从 HELLO 取设备名 */
inline std::string helloName(const uint8_t* pkt, int len) {
    const int hdr = (int)sizeof(PktHeader);
    if (len < hdr + 3) return std::string();
    const PktHeader* h = (const PktHeader*)pkt;
    int n = pkt[hdr + 2] & 0xFF;
    int body = (int)h->bodyLen;
    int extra = (body == 4 + n) ? 1 : 0;
    if (n > body - 3 - extra) n = (body > 3 + extra) ? body - 3 - extra : 0;
    if (n <= 0) return std::string();
    if (n > len - hdr - 3) n = len - hdr - 3;
    return std::string((const char*)(pkt + hdr + 3), (size_t)n);
}

inline int helloQuality(const uint8_t* pkt, int len) {
    const int hdr = (int)sizeof(PktHeader);
    if (len < hdr + 4) return -1;
    const PktHeader* h = (const PktHeader*)pkt;
    int n = pkt[hdr + 2] & 0xFF;
    if (h->bodyLen != (uint32_t)(4 + n) || hdr + 3 + n >= len) return -1;
    int q = pkt[hdr + 3 + n] & 0xFF;
    return q <= 2 ? q : -1;
}

inline bool validPacketHeader(const uint8_t* pkt, int len) {
    if (!pkt || len < (int)sizeof(PktHeader)) return false;
    const PktHeader* h = (const PktHeader*)pkt;
    return memcmp(h->magic, MAGIC, 4) == 0 && h->version == PROTO_VER
            && h->bodyLen <= CLIPBOARD_MAX_BYTES + 4096;
}

inline std::vector<uint8_t> buildClipboard(const std::string& text) {
    const size_t n = std::min(text.size(), CLIPBOARD_MAX_BYTES);
    std::vector<uint8_t> pkt(sizeof(PktHeader) + n);
    PktHeader* h = (PktHeader*)pkt.data();
    memcpy(h->magic, MAGIC, 4);
    h->version = PROTO_VER;
    h->type = PKT_CLIPBOARD;
    h->flags = 0;
    h->sourceId = 0;
    h->bodyLen = (uint32_t)n;
    if (n > 0) memcpy(pkt.data() + sizeof(PktHeader), text.data(), n);
    return pkt;
}

inline std::string parseClipboard(const uint8_t* pkt, int len) {
    if (!pkt || len < (int)sizeof(PktHeader)) return std::string();
    const PktHeader* h = (const PktHeader*)pkt;
    size_t n = std::min<size_t>(h->bodyLen, CLIPBOARD_MAX_BYTES);
    n = std::min<size_t>(n, (size_t)len - sizeof(PktHeader));
    return std::string((const char*)pkt + sizeof(PktHeader), n);
}

} // namespace ahub
