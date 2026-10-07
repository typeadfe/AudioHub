package com.audiohub.probe;

import android.content.Context;
import android.content.ClipData;
import android.content.ClipboardManager;

import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.Socket;

/** 接收端里的一路 WiFi 音频源。 */
final class PlayerSource {

    private static final long LIVE_TIMEOUT_MS = 1500;

    final String host;
    final int    port;
    private final Context ctx;
    private final String localName;
    private final int    localLevel;

    private volatile PlayerJitterBuffer jb;

    // ---- 源端格式（由 HELLO 决定）----
    private volatile int srcCodec      = Protocol.CODEC_PCM;
    private volatile int srcLevel      = Protocol.BW_FULL;
    private volatile int srcFrameMs    = Protocol.FRAME_MS;
    private volatile int srcFrameSamples = Protocol.FRAME_SAMPLES;  // 每声道
    private volatile int srcChannels   = Protocol.CHANNELS;

    private volatile boolean running   = false;
    private volatile int generation = 0;
    private volatile boolean connected = false;
    private volatile long    lastRecvMs = 0;
    private volatile String  status    = "等待连接";
    private volatile String  deviceName = "";
    private volatile int     authStatus = Protocol.AUTH_APPROVED;
    private volatile boolean remoteStreamEnabled = true;
    private Thread thread;
    private Thread statsThread;
    private AudioLink link;
    private Socket acceptedSocket;
    private boolean reverseInbound;
    private Runnable onClosed;
    private volatile OutputStream outStream;
    private final Object writeLock = new Object();

    volatile long bytes = 0;
    volatile boolean enabled = true;

    // ---- 接收端可调 ----
    volatile float   gain  = 1.0f;
    volatile boolean muted = false;

    // ---- 电平 ----
    volatile float levelDb      = -120f;
    volatile int   levelPercent = 0;

    // ---- 延迟测量 ----
    static volatile int outputBufferMs = 0;
    private volatile long minOffsetUs      = Long.MAX_VALUE;
    private volatile long minWindowStartMs = 0;
    private volatile long lastOffsetUs     = 0;
    volatile int latencyMs = 0;

    // ---- 渲染线程独占 ----
    private short[] fifo    = new short[Protocol.FRAME_SAMPLES * Protocol.CHANNELS * 64];
    private int     fifoFill = 0;
    private int     fifoPos  = 0;
    private final short[] pcmTmp = new short[8192];
    private final short[] upTmp  = new short[48000];   // 够 20ms@48k 立体声

    PlayerSource(String host, int port, Context ctx, String localName, int localLevel) {
        this.host  = host;
        this.port  = port;
        this.ctx   = ctx;
        this.localName  = (localName == null || localName.isEmpty()) ? "Android" : localName;
        this.localLevel = Protocol.clampLevel(localLevel);
        this.reverseInbound = false;
        this.jb = new PlayerJitterBuffer(Protocol.FRAME_MS);
    }

    PlayerSource(Socket accepted, Context ctx, String localName, int localLevel) {
        this(accepted.getInetAddress().getHostAddress(), Protocol.RECEIVER_PORT,
                ctx, localName, localLevel);
        this.acceptedSocket = accepted;
        this.reverseInbound = true;
    }

    void setOnClosed(Runnable callback) { onClosed = callback; }

    String displayName() {
        String n = deviceName;
        return (n == null || n.isEmpty()) ? host : n;
    }

    boolean isConnected()   { return connected; }
    boolean isRemoteStreamEnabled() { return remoteStreamEnabled; }
    String  status()        { return status; }
    int     pendingFrames() { return jb.pending(); }
    int     pendingMs()     { return jb.pendingMs(); }
    boolean isWarmedUp()    { return jb.warmedUp(); }

    String formatText() { return Protocol.bwFormatText(srcLevel); }

    boolean isActive() {
        return connected && (System.currentTimeMillis() - lastRecvMs) < LIVE_TIMEOUT_MS;
    }

    long statRecv()      { PlayerJitterBuffer b = jb; return (b == null) ? 0 : b.recvFrames; }
    long statLost()      { PlayerJitterBuffer b = jb; return (b == null) ? 0 : b.lostFrames; }
    long statUnderruns() { PlayerJitterBuffer b = jb; return (b == null) ? 0 : b.underruns; }

    // ------------------------------------------------------------------
    void start() {
        if (running) return;
        running = true;
        final int runId = ++generation;
        thread = new Thread(() -> loop(runId), "ahub-src-" + host);
        thread.start();
        statsThread = new Thread(() -> statsLoop(runId), "ahub-stats-" + host);
        statsThread.start();
    }

    void stop() {
        running = false;
        generation++;
        connected = false;
        try { if (link != null) link.close(); } catch (Exception ignored) {}
        try { if (acceptedSocket != null) acceptedSocket.close(); } catch (Exception ignored) {}
        acceptedSocket = null;
        try { if (thread != null) thread.join(800); } catch (InterruptedException ignored) {}
        try { if (statsThread != null) statsThread.join(500); } catch (InterruptedException ignored) {}
        thread = null;
        statsThread = null;
        outStream = null;
        resetStream();
    }

    private void statsLoop(int runId) {
        while (running && generation == runId) {
            try { Thread.sleep(1000); } catch (InterruptedException e) { return; }
            OutputStream os = outStream;
            if (os != null && connected) {
                try {
                    synchronized (writeLock) {
                        os.write(Protocol.buildStats(1, latencyMs));
                        os.flush();
                    }
                } catch (Exception ignored) {}
            }
        }
    }

    /** 将本机剪贴板发送回当前连接的发送端。 */
    boolean sendClipboard(String text) {
        OutputStream os = outStream;
        if (os == null || !connected) return false;
        try {
            synchronized (writeLock) {
                os.write(Protocol.buildClipboard(text == null ? "" : text));
                os.flush();
            }
            return true;
        } catch (Exception e) {
            return false;
        }
    }

    // ------------------------------------------------------------------
    private AudioLink openLink() throws Exception {
        if (reverseInbound) {
            Socket accepted = acceptedSocket;
            acceptedSocket = null;
            if (accepted == null) throw new java.io.IOException("反向连接已关闭");
            accepted.setTcpNoDelay(true);
            accepted.setReceiveBufferSize(256 * 1024);
            return new AudioLink.Tcp(accepted);
        }
        Socket s = new Socket();
        s.setTcpNoDelay(true);
        s.setReceiveBufferSize(256 * 1024);
        s.connect(new InetSocketAddress(host, port), 1500);
        return new AudioLink.Tcp(s);
    }

    private void loop(int runId) {
        // 接收线程负责解包与解码，被抢占会造成抖动缓冲见到不规律的到达节奏
        try {
            android.os.Process.setThreadPriority(
                    android.os.Process.THREAD_PRIORITY_URGENT_AUDIO);
        } catch (Throwable ignored) {}

        final byte[] buf = new byte[Protocol.CLIPBOARD_MAX_BYTES + Protocol.HDR_COMMON + 32];
        int fill = 0;

        while (running && generation == runId) {
            AudioLink lk = null;
            try {
                lk = openLink();
                if (!running || generation != runId) {
                    lk.close();
                    break;
                }
                link = lk;
                resetStream();
                remoteStreamEnabled = true;
                connected = true;
                status = "已连接";
                fill = 0;

                OutputStream out = lk.out();
                synchronized (writeLock) {
                    out.write(Protocol.buildHello(1, localName, localLevel, Protocol.CODEC_PCM));
                    out.flush();
                }
                outStream = out;

                InputStream in = lk.in();
                while (running && generation == runId) {
                    int r = in.read(buf, fill, buf.length - fill);
                    if (r <= 0) break;
                    fill += r;

                    while (fill >= Protocol.HDR_COMMON) {
                        if (!Protocol.validHeader(buf, fill)) {
                            int at = resync(buf, fill);
                            if (at < 0) { fill = 0; break; }
                            System.arraycopy(buf, at, buf, 0, fill - at);
                            fill -= at;
                            continue;
                        }
                        byte type = Protocol.typeOf(buf);

                        // ---- HELLO：编码方式与带宽档位 ----
                        if (type == Protocol.TYPE_HELLO) {
                            int need = Protocol.HDR_COMMON + Protocol.bodyLenOf(buf);
                            if (need <= 0 || need > buf.length) { fill = 0; break; }
                            if (fill < need) break;
                            int codec = Protocol.parseHelloCodec(buf, fill);
                            int lv    = Protocol.parseHelloLevel(buf, fill);
                            int quality = Protocol.parseHelloQuality(buf, fill);
                            String n  = Protocol.parseHelloName(buf, fill);
                            applySourceFormat(codec, lv);
                            if (quality >= 0) jb.setTargetMsInstance(quality == 0 ? 20
                                    : quality == 2 ? 80 : 40);
                            if (!n.isEmpty()) {
                                deviceName = n;
                                CaptureState.log("[接收] 已连接音源：" + n + "（WiFi · "
                                        + Protocol.bwFormatText(lv) + "，"
                                        + Protocol.bwKbps(lv) + " kbps）");
                            }
                            System.arraycopy(buf, need, buf, 0, fill - need);
                            fill -= need;
                            continue;
                        }

                        // ---- AUTH：发送端的授权状态 ----
                        if (type == Protocol.TYPE_AUTH) {
                            int need = Protocol.HDR_COMMON + 1;
                            if (fill < need) break;
                            int st = Protocol.parseAuth(buf, fill);
                            if (st >= 0) {
                                authStatus = st;
                                if (st == Protocol.AUTH_PENDING) {
                                    status = "等待发送端确认";
                                } else if (st == Protocol.AUTH_DENIED) {
                                    status = "已被发送端拒绝";
                                } else {
                                    status = "已连接";
                                }
                            }
                            System.arraycopy(buf, need, buf, 0, fill - need);
                            fill -= need;
                            continue;
                        }

                        if (type == Protocol.TYPE_STREAM_STATE) {
                            int need = Protocol.HDR_COMMON + Protocol.bodyLenOf(buf);
                            if (need > buf.length || need < Protocol.HDR_COMMON + 1) { fill = 0; break; }
                            if (fill < need) break;
                            remoteStreamEnabled = buf[Protocol.HDR_COMMON] != 0;
                            if (!remoteStreamEnabled) resetStream();
                            System.arraycopy(buf, need, buf, 0, fill - need);
                            fill -= need;
                            continue;
                        }

                        // ---- CLIPBOARD：发送端发来的文本自动写入本机剪贴板 ----
                        if (type == Protocol.TYPE_CLIPBOARD) {
                            int need = Protocol.HDR_COMMON + Protocol.bodyLenOf(buf);
                            if (need < Protocol.HDR_COMMON || need > buf.length) { fill = 0; break; }
                            if (fill < need) break;
                            String text = Protocol.parseClipboard(buf, fill);
                            ClipboardManager cm = (ClipboardManager)
                                    ctx.getSystemService(Context.CLIPBOARD_SERVICE);
                            if (cm != null && authStatus == Protocol.AUTH_APPROVED) {
                                cm.setPrimaryClip(ClipData.newPlainText("AudioHub", text));
                                ClipboardNotice.show(ctx, UiText.tr(ctx, "已收到来自 ", "Clipboard received from ")
                                        + displayName() + UiText.tr(ctx, " 的剪贴板内容", ""));
                                CaptureState.log("[剪贴板] 已接收来自 " + displayName()
                                        + " 的内容（" + text.length() + " 字符）");
                            }
                            System.arraycopy(buf, need, buf, 0, fill - need);
                            fill -= need;
                            continue;
                        }

                        // ---- AUDIO：长度由包头 bodyLen 决定----
                        if (type == Protocol.TYPE_AUDIO) {
                            int body = Protocol.bodyLenOf(buf);
                            int pkt  = Protocol.HDR_COMMON + body;
                            if (pkt <= 0 || pkt > buf.length) { fill = 0; break; }
                            if (fill < pkt) break;

                            int seq = Protocol.getShortLE(buf, Protocol.HDR_COMMON);
                            byte[] payload = new byte[body - Protocol.HDR_AUDIO];
                            System.arraycopy(buf, Protocol.HDR_TOTAL, payload, 0, payload.length);
                            jb.push(seq, payload);
                            lastRecvMs = System.currentTimeMillis();
                            bytes += pkt;

                            // 延迟测量（须在 arraycopy 之前读 buffer）
                            {
                                long myUs  = (System.nanoTime() / 1000) & 0xFFFFFFFFL;
                                long srcUs = Protocol.getIntLE(buf, Protocol.HDR_COMMON + 4) & 0xFFFFFFFFL;
                                long d     = Protocol.clockDiffUs(myUs, srcUs);
                                long nowMs = System.currentTimeMillis();
                                if (minOffsetUs == Long.MAX_VALUE || nowMs - minWindowStartMs > 30000L) {
                                    minWindowStartMs = nowMs;
                                    minOffsetUs = d;
                                }
                                if (d < minOffsetUs) minOffsetUs = d;
                                lastOffsetUs = d;
                            }

                            System.arraycopy(buf, pkt, buf, 0, fill - pkt);
                            fill -= pkt;
                            continue;
                        }

                        // ---- 其他类型：按 bodyLen 跳过 ----
                        int need = Protocol.HDR_COMMON + Protocol.bodyLenOf(buf);
                        if (need <= 0 || need > fill) { fill = 0; break; }
                        System.arraycopy(buf, need, buf, 0, fill - need);
                        fill -= need;
                    }
                }
            } catch (Exception e) {
                if (running && generation == runId) {
                    status = "连接失败，重试中";
                }
            } finally {
                try { if (lk != null) lk.close(); } catch (Exception ignored) {}
                if (generation == runId) {
                    connected = false;
                    outStream = null;
                    link = null;
                    resetStream();
                }
            }
            if (reverseInbound) break;
            if (running && generation == runId) {
                status = "已断开，重连中";
                try { Thread.sleep(5000); } catch (InterruptedException e) { break; }
            }
        }
        if (generation == runId) {
            connected = false;
            status = "已停止";
        }
        if (reverseInbound && onClosed != null) onClosed.run();
    }

    /** 源端格式变化时重建抖动缓冲。音频统一为 WiFi PCM。 */
    private synchronized void applySourceFormat(int codec, int level) {
        srcCodec = Protocol.CODEC_PCM;
        srcLevel = Protocol.clampLevel(level);
        srcFrameMs = Protocol.FRAME_MS;
        srcFrameSamples = Protocol.bwFrameSamples(srcLevel);
        srcChannels = Protocol.bwChannels(srcLevel);
        jb = new PlayerJitterBuffer(srcFrameMs);
        fifoFill = 0;
        fifoPos = 0;
    }

    /** A new connection must not reuse decoded audio or codec state from the old stream. */
    private synchronized void resetStream() {
        srcCodec = Protocol.CODEC_PCM;
        srcLevel = Protocol.BW_FULL;
        srcFrameMs = Protocol.FRAME_MS;
        srcFrameSamples = Protocol.FRAME_SAMPLES;
        srcChannels = Protocol.CHANNELS;
        jb = new PlayerJitterBuffer(Protocol.FRAME_MS);
        fifoFill = fifoPos = 0;
        lastRecvMs = 0;
        minOffsetUs = Long.MAX_VALUE;
        minWindowStartMs = lastOffsetUs = 0;
        latencyMs = 0;
        levelPercent = 0;
    }

    private static int resync(byte[] b, int len) {
        for (int i = 1; i + 5 <= len; i++) {
            if (b[i] == 'A' && b[i + 1] == 'H' && b[i + 2] == 'U' && b[i + 3] == 'B'
                    && (b[i + 4] & 0xFF) == Protocol.VERSION) {
                return i;
            }
        }
        return -1;
    }

    /**
     * 把一帧负载解码成 48kHz 立体声（交错）。
     * PCM：解析 16bit 小端后上采样/声道扩展到 48kHz 立体声。
     * 返回写入的样本数（含所有声道）。
     */
    private int decodeTo48kStereo(byte[] payload, short[] out) {
        int n = Math.min(payload.length / 2, pcmTmp.length);
        for (int i = 0; i < n; i++) {
            int lo = payload[i * 2] & 0xFF;
            int hi = payload[i * 2 + 1];
            pcmTmp[i] = (short) ((hi << 8) | lo);
        }
        return upsamplePcm(pcmTmp, n);
    }

    private int upsamplePcm(short[] in, int inSamples) {
        final int outN = Protocol.FRAME_SAMPLES;   // 每声道 240
        final int outLen = outN * Protocol.CHANNELS;

        if (inSamples <= 0) {
            java.util.Arrays.fill(upTmp, 0, outLen, (short) 0);
            return outLen;
        }
        int perChannel = inSamples / srcChannels;
        // Only a full 48 kHz stereo frame may be copied directly.
        if (srcChannels == 2 && perChannel == outN) {
            System.arraycopy(in, 0, upTmp, 0, outLen);
            return outLen;
        }
        if (srcChannels == 2) {
            // 降过采样率的立体声：两个声道各自线性插值回 48kHz。
            // 早期这里只处理了 48kHz 的情况，低采样率的立体声会被直接拷贝，
            // 结果播放速度错误 —— 加了"高保真/标准保持立体声"之后必须补上。
            final double step = (double) perChannel / outN;
            for (int j = 0; j < outN; j++) {
                double sPos = j * step;
                int i0 = (int) sPos;
                if (i0 >= perChannel) i0 = perChannel - 1;
                int i1 = (i0 + 1 < perChannel) ? i0 + 1 : perChannel - 1;
                float f = (float) (sPos - i0);
                upTmp[j * 2]     = interp(in[i0 * 2],     in[i1 * 2],     f);
                upTmp[j * 2 + 1] = interp(in[i0 * 2 + 1], in[i1 * 2 + 1], f);
            }
            return outLen;
        }
        // 单声道 → 上采样并复制成双声道
        final double step = (double) inSamples / outN;
        for (int j = 0; j < outN; j++) {
            double sPos = j * step;
            int i0 = (int) sPos;
            if (i0 >= inSamples) i0 = inSamples - 1;
            int i1 = (i0 + 1 < inSamples) ? i0 + 1 : inSamples - 1;
            float f = (float) (sPos - i0);
            short sv = interp(in[i0], in[i1], f);
            upTmp[j * 2] = sv;
            upTmp[j * 2 + 1] = sv;
        }
        return outLen;
    }

    /** 线性插值并夹到 16bit 范围 */
    private static short interp(short a, short b, float f) {
        float v = a + (b - a) * f;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        return (short) Math.round(v);
    }

    // ------------------------------------------------------------------
    /** 把本路（乘 gain）叠加到 acc。只由渲染线程调用 */
    synchronized void mixInto(float[] acc, int frames) {
        if (!connected) { levelPercent = 0; return; }
        long now = System.currentTimeMillis();
        if (now - lastRecvMs > LIVE_TIMEOUT_MS) { levelPercent = 0; return; }

        final int need = frames * Protocol.CHANNELS;

        while (fifoFill - fifoPos < need) {
            byte[] payload = jb.pop();
            int got = 0;
            if (payload != null) {
                got = decodeTo48kStereo(payload, upTmp);
            }
            if (got <= 0) {
                // 链路停顿时填充静音，避免重复旧音频。
                // Repeating a tonal frame for up to LIVE_TIMEOUT_MS caused the "woo" sound.
                ensureCapacity(Protocol.FRAME_SAMPLES * Protocol.CHANNELS);
                java.util.Arrays.fill(fifo, fifoFill,
                        fifoFill + Protocol.FRAME_SAMPLES * Protocol.CHANNELS, (short) 0);
                fifoFill += Protocol.FRAME_SAMPLES * Protocol.CHANNELS;
            } else {
                ensureCapacity(got);
                System.arraycopy(upTmp, 0, fifo, fifoFill, got);
                fifoFill += got;
            }
        }

        if (AppState.metersVisible) {
            long sumSq = 0;
            for (int i = 0; i < need; i++) {
                int v = fifo[fifoPos + i];
                sumSq += (long) v * v;
            }
            double rms = Math.sqrt((double) sumSq / need);
            levelDb = rms > 0 ? (float) (20.0 * Math.log10(rms / 32768.0)) : -120f;
            int pct = (int) (rms / 32768.0 * 400.0);
            if (pct > 100) pct = 100;
            if (pct < 0) pct = 0;
            levelPercent = pct;
        }

        // 估算延迟 = 抖动缓冲水位 + 网络额外延迟 + 输出缓冲
        int bufMs = jb.pendingMs()
                + (fifoFill - fifoPos) / Protocol.CHANNELS * 1000 / Protocol.SAMPLE_RATE;
        int netExtra = 0;
        if (minOffsetUs != Long.MAX_VALUE) {
            long e = (lastOffsetUs - minOffsetUs) / 1000L;
            if (e > 0) netExtra = (int) Math.min(e, 500L);
        }
        latencyMs = bufMs + netExtra + outputBufferMs;

        final float g = muted ? 0f : gain;
        if (g != 0f) {
            for (int i = 0; i < need; i++) acc[i] += fifo[fifoPos + i] * g;
        }

        fifoPos += need;
        if (fifoPos >= fifo.length / 2) {
            int rest = fifoFill - fifoPos;
            if (rest > 0) System.arraycopy(fifo, fifoPos, fifo, 0, rest);
            fifoFill = rest;
            fifoPos = 0;
        }
    }

    private void ensureCapacity(int extra) {
        if (fifoFill + extra <= fifo.length) return;
        int want = Math.max(fifo.length * 2, fifoFill + extra);
        short[] nf = new short[want];
        System.arraycopy(fifo, 0, nf, 0, fifoFill);
        fifo = nf;
    }
}
