package com.audiohub.probe;

/**
 * 协议定义 —— 必须与 Windows 端 windows-player/src/common.h 完全一致。
 *
 * ⚠️ 所有多字节字段一律【小端序 little-endian】。
 *    x86_64(Windows) 与 ARM64(Android) 都是小端，两端结构体可直接内存映射。
 *
 * 包结构（音频包，共 984 字节）：
 *   偏移  长度  字段
 *   0     4     magic "AHUB"
 *   4     1     version
 *   5     1     type
 *   6     2     flags
 *   8     4     sourceId
 *   12    4     bodyLen          = 8 + 960 = 968
 *   ---- 以上为通用包头 16 字节 ----
 *   16    2     seq
 *   18    2     frameSamples     = 240
 *   20    4     timestampUs
 *   ---- 以上为音频包头，合计 24 字节 ----
 *   24    960   PCM16 交错立体声
 */
public final class Protocol {

    private Protocol() {}

    public static final int SAMPLE_RATE  = 48000;
    public static final int CHANNELS     = 2;
    public static final int FRAME_MS     = 5;
    public static final int FRAME_SAMPLES = SAMPLE_RATE * FRAME_MS / 1000;   // 240
    public static final int FRAME_BYTES  = FRAME_SAMPLES * CHANNELS * 2;     // 960
    /** 手机接收端监听，供电脑发送端主动建立出站连接。 */
    public static final int RECEIVER_PORT = 53536;

    public static final int HDR_COMMON   = 16;
    public static final int HDR_AUDIO    = 8;
    public static final int HDR_TOTAL    = HDR_COMMON + HDR_AUDIO;           // 24
    public static final int PKT_BYTES    = HDR_TOTAL + FRAME_BYTES;          // 984

    public static final byte VERSION       = 1;
    public static final byte TYPE_DISCOVER = 0x01;
    public static final byte TYPE_REPLY    = 0x02;
    public static final byte TYPE_BYE      = 0x03;
    /** 握手：连接建立后双方各发一次，负载是 UTF-8 设备显示名 */
    public static final byte TYPE_HELLO    = 0x04;
    /** 延迟上报：接收端周期性把测得的延迟告诉发送端，负载 2 字节（毫秒，小端） */
    public static final byte TYPE_STATS    = 0x05;
    /**
     * 接收端存在广播（UDP，接收端 → 局域网）。
     *
     * 为什么需要它：连接方向是"接收端主动连发送端"（为绕开 Windows 防火墙入站拦截），
     * 因此发送端【无法主动发现】接收端 —— 对方不连过来，发送端就不知道有谁在。
     * 让接收端周期性广播自己的存在，发送端就能列出"可用的接收端"，
     * 而不是只能看着一个空列表等对方主动连。
     */
    public static final byte TYPE_ANNOUNCE = 0x20;
    /**
     * 授权状态通知：发送端 → 接收端，负载 1 字节。
     *
     * 这是安全机制的一部分：接收端连上后不会立刻收到音频，
     * 必须由发送端本人在界面上点【允许】才会开始发送。
     * 一个几十行的脚本能建立 TCP 连接、能收到 HELLO，但拿不到任何音频。
     */
    public static final byte TYPE_AUTH     = 0x06;
    /** 剪贴板同步：UTF-8 文本负载，双向 TCP 控制包。 */
    public static final byte TYPE_CLIPBOARD = 0x30;
    public static final byte TYPE_STREAM_STATE = 0x31;
    public static final int CLIPBOARD_MAX_BYTES = 64 * 1024;

    /** 授权状态取值 */
    public static final int AUTH_PENDING  = 0;   // 等待发送端用户批准
    public static final int AUTH_APPROVED = 1;   // 已批准，开始发送
    public static final int AUTH_DENIED   = 2;   // 已被拒绝
    public static final byte TYPE_AUDIO    = 0x10;

    public static final int DISCOVERY_PORT = 53535;
    /** 接收端存在广播用的 UDP 端口（与音频 TCP 端口区分开） */
    public static final int ANNOUNCE_PORT  = 53537;

    /** 发现包间隔（毫秒） */
    public static final int DISCOVER_INTERVAL_MS = 1000;

    // ---------------- 抖动缓冲参数（与 Windows 端保持一致） ----------------
    /** 目标水位（毫秒）：预热到这个值才开始播 */
    public static final int JITTER_TARGET_MS = 40;
    /** 高水位：超过就丢一帧往前追，避免延迟累积 */
    public static final int JITTER_HIGH_MS   = 80;
    /** 低水位：低于就重复上一帧填补，避免爆音 */
    public static final int JITTER_LOW_MS    = 20;

    /** 播放端输出采样率（与源一致，避免重采样） */
    public static final int PLAY_SAMPLE_RATE = SAMPLE_RATE;

    // ================== 带宽档位 ==================
    //
    // 用【采样率 + 声道】实现，不引入编解码器，两端都不需要额外依赖。
    //
    // 而 48kHz 立体声无损 PCM 需要 1.54 Mbps —— 单路都塞不下。
    //
    // 档位            采样率   声道    抽取倍数   有效载荷带宽
    //   BW_FULL       48000    2        1       1536 kbps   完全无损
    //   BW_HIGH       24000    2        2        768 kbps   高频上限 12kHz
    //   BW_MED        16000    2        3        512 kbps   高频上限 8kHz
    //   BW_LOW        16000    1        3        256 kbps   仅此档为单声道
    //
    // 【为什么只有最低档才是单声道】
    // 立体声降到单声道会把左右声道直接相加，声场和空间感完全消失，
    // 对游戏（听声辨位）和音乐影响都很大 —— 这比降采样率的损失明显得多。
    // 所以优先砍采样率，把单声道留到最后不得已的一档。
    //
    // 48000/24000/16000 之间都是整数倍关系（1/2/3），
    // 因此可以用"组内平均"做抽取，不需要任意比例重采样。
    public static final int BW_FULL = 0;
    public static final int BW_HIGH = 1;
    public static final int BW_MED  = 2;
    public static final int BW_LOW  = 3;
    public static final int BW_COUNT = 4;
    public static int clampLevel(int level) {
        if (level < 0) return BW_FULL;
        if (level >= BW_COUNT) return BW_COUNT - 1;
        return level;
    }
    public static int bwSampleRate(int level) {
        switch (clampLevel(level)) {
            case BW_HIGH: return 24000;
            case BW_MED:  return 16000;
            case BW_LOW:  return 16000;
            default:      return 48000;
        }
    }
    public static int bwChannels(int level) { return clampLevel(level) == BW_LOW ? 1 : 2; }
    public static int bwFrameSamples(int level) { return bwSampleRate(level) * FRAME_MS / 1000; }
    public static int bwDecimation(int level) {
        switch (clampLevel(level)) {
            case BW_HIGH: return 2;
            case BW_MED:
            case BW_LOW: return 3;
            default: return 1;
        }
    }
    public static int bwKbps(int level) {
        return bwSampleRate(level) * bwChannels(level) * 16 / 1000;
    }
    public static String bwLabel(int level) {
        switch (clampLevel(level)) {
            case BW_HIGH: return "高保真";
            case BW_MED: return "标准";
            case BW_LOW: return "省流";
            default: return "无损原音";
        }
    }
    public static String bwFormatText(int level) {
        return bwSampleRate(level) / 1000 + " kHz "
                + (bwChannels(level) == 1 ? "单声道" : "立体声");
    }

    // ================== WiFi PCM 音频 ==================
    public static final int CODEC_PCM = 0;

    public static int clampCodec(int c) { return CODEC_PCM; }
    public static int frameMsFor(int codec) { return FRAME_MS; }
    public static int sampleRateFor(int codec, int level) { return bwSampleRate(level); }
    public static int channelsFor(int codec, int level) { return bwChannels(level); }
    public static int frameSamplesFor(int codec, int level) {
        return sampleRateFor(codec, level) * FRAME_MS / 1000;
    }
    public static int pcmFrameBytesFor(int codec, int level) {
        return frameSamplesFor(codec, level) * channelsFor(codec, level) * 2;
    }
    public static int framesPerSecond(int codec) { return 1000 / FRAME_MS; }

    public static int getShortLE(byte[] b, int o) {
        return (b[o] & 0xFF) | ((b[o + 1] & 0xFF) << 8);
    }
    public static int getIntLE(byte[] b, int o) {
        return (b[o] & 0xFF) | ((b[o + 1] & 0xFF) << 8)
                | ((b[o + 2] & 0xFF) << 16) | ((b[o + 3] & 0xFF) << 24);
    }
    public static void putShortLE(byte[] b, int o, int v) {
        b[o] = (byte) v; b[o + 1] = (byte) (v >>> 8);
    }
    public static void putIntLE(byte[] b, int o, int v) {
        b[o] = (byte) v; b[o + 1] = (byte) (v >>> 8);
        b[o + 2] = (byte) (v >>> 16); b[o + 3] = (byte) (v >>> 24);
    }
    public static boolean validHeader(byte[] b, int len) {
        return len >= HDR_COMMON && b[0] == 'A' && b[1] == 'H'
                && b[2] == 'U' && b[3] == 'B' && b[4] == VERSION
                && bodyLenOf(b) >= 0 && bodyLenOf(b) <= 1024 * 1024;
    }
    public static byte typeOf(byte[] b) { return b[5]; }
    public static int  sourceIdOf(byte[] b) { return getIntLE(b, 8); }
    public static int  bodyLenOf(byte[] b) { return getIntLE(b, 12); }
    public static int  dataPortOf(byte[] b) { return getShortLE(b, HDR_COMMON); }

    /**
     * 写入通用包头。
     * 注意：长度字段用 int 计算，绝不能用 (int) 截断后的比较来判断合法性
     * （Windows 端曾因字节序错误导致长度溢出成负数、校验意外通过，排查很久）。
     */
    public static void writeHeader(byte[] pkt, byte type, int sourceId, int bodyLen) {
        pkt[0] = 'A'; pkt[1] = 'H'; pkt[2] = 'U'; pkt[3] = 'B';
        pkt[4] = VERSION;
        pkt[5] = type;
        putShortLE(pkt, 6, 0);          // flags
        putIntLE(pkt, 8, sourceId);
        putIntLE(pkt, 12, bodyLen);
    }

    /**
     * 构造 HELLO 包。
     * 负载格式：[编码方式 1 字节][带宽档位 1 字节][名称长度 1 字节][UTF-8 名称]
     *
     * 编码方式与带宽档位都必须在握手时告知对方：
     * 它们共同决定音频帧的长度、采样数与声道数，接收端要据此配置抖动缓冲与解码器。
     */
    public static byte[] buildHello(int sourceId, String name, int bwLevel, int codec) {
        return buildHello(sourceId, name, bwLevel, codec, -1);
    }

    /** HELLO with sender-selected jitter quality: 0=20ms, 1=40ms, 2=80ms. */
    public static byte[] buildHello(int sourceId, String name, int bwLevel, int codec, int quality) {
        byte[] nb = (name == null) ? new byte[0]
                : name.getBytes(java.nio.charset.StandardCharsets.UTF_8);
        if (nb.length > 60) {
            byte[] t = new byte[60];
            System.arraycopy(nb, 0, t, 0, 60);
            nb = t;
        }
        boolean hasQuality = quality >= 0 && quality <= 2;
        byte[] pkt = new byte[HDR_COMMON + 3 + nb.length + (hasQuality ? 1 : 0)];
        writeHeader(pkt, TYPE_HELLO, sourceId, 3 + nb.length + (hasQuality ? 1 : 0));
        pkt[HDR_COMMON]     = (byte) CODEC_PCM;
        pkt[HDR_COMMON + 1] = (byte) clampLevel(bwLevel);
        pkt[HDR_COMMON + 2] = (byte) nb.length;
        System.arraycopy(nb, 0, pkt, HDR_COMMON + 3, nb.length);
        if (hasQuality) pkt[HDR_COMMON + 3 + nb.length] = (byte) quality;
        return pkt;
    }

    /** 从 HELLO 取编码方式；无效按 PCM 处理 */
    public static int parseHelloCodec(byte[] pkt, int len) {
        if (len < HDR_COMMON + 1) return CODEC_PCM;
        return CODEC_PCM;
    }

    /** 从 HELLO 取带宽档位；无效则按无损处理 */
    public static int parseHelloLevel(byte[] pkt, int len) {
        if (len < HDR_COMMON + 2) return BW_FULL;
        return clampLevel(pkt[HDR_COMMON + 1] & 0xFF);
    }

    /** 从 HELLO 解析设备名 */
    public static String parseHelloName(byte[] pkt, int len) {
        if (len < HDR_COMMON + 3) return "";
        int body = bodyLenOf(pkt);
        int n = pkt[HDR_COMMON + 2] & 0xFF;
        int extra = body >= 4 && n == body - 4 ? 1 : 0;
        if (n > body - 3 - extra) n = Math.max(0, body - 3 - extra);
        if (n <= 0) return "";
        int avail = Math.min(n, len - HDR_COMMON - 3);
        if (avail <= 0) return "";
        try {
            return new String(pkt, HDR_COMMON + 3, avail,
                    java.nio.charset.StandardCharsets.UTF_8).trim();
        } catch (Exception e) {
            return "";
        }
    }

    public static int parseHelloQuality(byte[] pkt, int len) {
        if (len < HDR_COMMON + 4) return -1;
        int body = bodyLenOf(pkt);
        int n = pkt[HDR_COMMON + 2] & 0xFF;
        if (body == 4 + n && HDR_COMMON + 3 + n < len) {
            int q = pkt[HDR_COMMON + 3 + n] & 0xFF;
            return q <= 2 ? q : -1;
        }
        return -1;
    }

    /** 构造延迟上报包 */
    public static byte[] buildStats(int sourceId, int latencyMs) {
        int v = latencyMs;
        if (v < 0) v = 0;
        if (v > 65535) v = 65535;
        byte[] pkt = new byte[HDR_COMMON + 2];
        writeHeader(pkt, TYPE_STATS, sourceId, 2);
        putShortLE(pkt, HDR_COMMON, v);
        return pkt;
    }

    /** 解析延迟上报，返回毫秒；无效返回 -1 */
    public static int parseStats(byte[] pkt, int len) {
        if (len < HDR_COMMON + 2) return -1;
        return getShortLE(pkt, HDR_COMMON);
    }

    /** 构造授权状态包 */
    public static byte[] buildAuth(int sourceId, int status) {
        byte[] pkt = new byte[HDR_COMMON + 1];
        writeHeader(pkt, TYPE_AUTH, sourceId, 1);
        pkt[HDR_COMMON] = (byte) status;
        return pkt;
    }

    /** 解析授权状态；无效返回 -1 */
    public static int parseAuth(byte[] pkt, int len) {
        if (len < HDR_COMMON + 1) return -1;
        return pkt[HDR_COMMON] & 0xFF;
    }

    public static byte[] buildStreamState(boolean enabled) {
        byte[] pkt = new byte[HDR_COMMON + 1];
        writeHeader(pkt, TYPE_STREAM_STATE, 0, 1);
        pkt[HDR_COMMON] = (byte) (enabled ? 1 : 0);
        return pkt;
    }

    /** 构造剪贴板文本包；超出 64 KiB 按 UTF-8 字节截断。 */
    public static byte[] buildClipboard(String text) {
        byte[] raw = (text == null ? "" : text)
                .getBytes(java.nio.charset.StandardCharsets.UTF_8);
        if (raw.length > CLIPBOARD_MAX_BYTES) {
            byte[] t = new byte[CLIPBOARD_MAX_BYTES];
            System.arraycopy(raw, 0, t, 0, t.length);
            raw = t;
        }
        byte[] pkt = new byte[HDR_COMMON + raw.length];
        writeHeader(pkt, TYPE_CLIPBOARD, 0, raw.length);
        System.arraycopy(raw, 0, pkt, HDR_COMMON, raw.length);
        return pkt;
    }

    /** 解析剪贴板文本包。 */
    public static String parseClipboard(byte[] pkt, int len) {
        if (len < HDR_COMMON) return "";
        int body = bodyLenOf(pkt);
        int n = Math.min(Math.min(body, CLIPBOARD_MAX_BYTES), len - HDR_COMMON);
        if (n <= 0) return "";
        try {
            return new String(pkt, HDR_COMMON, n,
                    java.nio.charset.StandardCharsets.UTF_8);
        } catch (Exception e) {
            return "";
        }
    }

    /** 构造"接收端存在"广播包（负载：UTF-8 设备名） */
    public static byte[] buildAnnounce(String name) {
        byte[] nb = (name == null) ? new byte[0]
                : name.getBytes(java.nio.charset.StandardCharsets.UTF_8);
        if (nb.length > 60) {
            byte[] t = new byte[60];
            System.arraycopy(nb, 0, t, 0, 60);
            nb = t;
        }
        byte[] pkt = new byte[HDR_COMMON + nb.length];
        writeHeader(pkt, TYPE_ANNOUNCE, 0, nb.length);
        System.arraycopy(nb, 0, pkt, HDR_COMMON, nb.length);
        return pkt;
    }

    /** 从"接收端存在"广播包解析设备名 */
    public static String parseAnnounceName(byte[] pkt, int len) {
        int body = bodyLenOf(pkt);
        int n = Math.min(body, len - HDR_COMMON);
        if (n <= 0) return "";
        try {
            return new String(pkt, HDR_COMMON, n,
                    java.nio.charset.StandardCharsets.UTF_8).trim();
        } catch (Exception e) {
            return "";
        }
    }

    /**
     * 计算源时间戳与本地时间的差值（微秒，32 位回绕安全）。
     *
     * timestampUs 是 uint32 微秒（约 71 分钟回绕），因此本地时钟也截断到 32 位后
     * 做模差，再折成有符号值。只要真实差值不超过 ±35 分钟就是正确的。
     */
    public static long clockDiffUs(long localUs, long srcTsUs) {
        long a = localUs & 0xFFFFFFFFL;
        long b = srcTsUs & 0xFFFFFFFFL;
        long d = (a - b) & 0xFFFFFFFFL;
        if (d > 0x80000000L) d -= 0x100000000L;
        return d;
    }
}
