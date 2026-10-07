package com.audiohub.probe;

import android.content.Context;
import android.content.ClipData;
import android.content.ClipboardManager;

import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.atomic.AtomicLong;

/** WiFi 音频服务端；接收端主动建立 TCP 连接，音频统一使用 PCM。 */
final class AudioServer {

    /** 限制待发送音频队列，避免网络抖动时延迟无限增长。 */
    private static final int QUEUE_FRAMES = 16;

    private ServerSocket server;
    private Thread acceptThread;
    private Thread authThread;
    private volatile boolean authRunning = false;
    private volatile boolean running = false;
    private String localName = "Android";
    private int    bwLevel   = Protocol.BW_FULL;
    private Context ctx;
    private boolean approvalRequired = true;

    private final List<Client> clients = new CopyOnWriteArrayList<>();
    private final AtomicLong framesSent    = new AtomicLong();
    private final AtomicLong framesDropped = new AtomicLong();
    private final AtomicLong bytesSent     = new AtomicLong();

    /** 一个已连接的接收端 */
    static final class Client {
        final AudioLink link;
        final OutputStream out;
        final InputStream  in;
        /**
         * 待编码的 48kHz 立体声 PCM 帧。
         * 采集线程只往里放 PCM，writeLoop 取出来做转换/编码后写出 ——
         * 这样 MediaCodec 的阻塞不会拖垮采集节奏。
         */
        final ArrayBlockingQueue<short[]> pcmQueue = new ArrayBlockingQueue<>(QUEUE_FRAMES);
        /** 控制包（HELLO / AUTH），优先于音频发出 */
        final ArrayBlockingQueue<byte[]> ctlQueue = new ArrayBlockingQueue<>(8);
        final Thread writer;
        final Thread reader;
        final String addr;
        final int    sourceId;
        final String localName;
        final AtomicLong totalBytesSent;
        final AtomicLong totalFramesDropped;

        /**
         * 是否已收到对方的 HELLO。
         * 必须靠它区分【真实接收端】和【扫描探测】—— 扫描是"连一下再关掉"，
         * 在 TCP 层无法与真实连接分辨。未握手的连接不显示、不发音频、3 秒后关闭。
         */
        volatile boolean hasHello = false;

        volatile String  name;
        volatile boolean enabled = true;
        volatile boolean alive   = true;
        volatile int     latencyMs = -1;

        /** 授权状态（见 Protocol.AUTH_*）：只有 AUTH_APPROVED 才会真正收到音频 */
        volatile int     auth = Protocol.AUTH_PENDING;

        /** 本连接的编码方式与带宽档位 */
        volatile int codec = Protocol.CODEC_PCM;
        volatile int level = Protocol.BW_FULL;

        final long createdMs = System.currentTimeMillis();
        final AtomicLong dropped = new AtomicLong();

        // ---- 每连接的重采样 / 编码状态 ----
        private int      frameMs;
        private int      need48;          // 一帧对应的 48k 每声道样本数
        private short[]  accum;           // 48k 立体声累积（交错）
        private int      accumFill = 0;   // 以 short 计
        private short[]  convBuf;         // 转换后的目标格式 PCM
        private byte[]   pkt;
        private int      sendSeq = 0;
        private volatile int requestedLevel = -1;
        private volatile int requestedQuality = -1;

        Client(AudioLink link, int sourceId, String localName, int bwLevel,
               Context ctx, boolean approvalRequired, AtomicLong totalBytesSent,
               AtomicLong totalFramesDropped) throws Exception {
            this.link = link;
            this.sourceId = sourceId;
            this.localName = localName;
            this.totalBytesSent = totalBytesSent;
            this.totalFramesDropped = totalFramesDropped;
            this.ctx = ctx;
            this.approvalRequired = approvalRequired;
            this.addr = link.remoteAddr();
            this.name = addr;
            this.out = link.out();
            this.in  = link.in();

            this.codec = Protocol.CODEC_PCM;
            this.level = bwLevel;
            reconfigure(codec, level);
            if (!approvalRequired) auth = Protocol.AUTH_APPROVED;

            try {
                out.write(Protocol.buildHello(sourceId, localName, this.level, this.codec,
                        Settings.quality(ctx)));
                out.write(Protocol.buildAuth(sourceId, auth));
                out.flush();
            } catch (Exception ignored) {}

            writer = new Thread(this::writeLoop, "ahub-tx-" + addr);
            writer.start();
            reader = new Thread(this::readLoop, "ahub-rx-" + addr);
            reader.start();
        }

        private final Context ctx;
        private final boolean approvalRequired;

        /** 按编码方式与档位重算帧长、缓冲与编解码器 */
        synchronized void reconfigure(int codec, int level) {
            this.codec = Protocol.CODEC_PCM;
            this.level = Protocol.clampLevel(level);
            this.frameMs = Protocol.FRAME_MS;
            this.need48  = 48000 * frameMs / 1000;                 // 每声道 48k 样本
            if (accum == null || accum.length < need48 * 2) accum = new short[need48 * 2];
            accumFill = 0;

            int outSamples = Protocol.frameSamplesFor(this.codec, this.level);
            int ch         = Protocol.channelsFor(this.codec, this.level);
            convBuf = new short[outSamples * ch];
            pkt     = new byte[Protocol.HDR_TOTAL + 4096];
        }

        private void writeLoop() {
            // 格式转换在发送线程完成，避免阻塞采集线程。
            try {
                android.os.Process.setThreadPriority(
                        android.os.Process.THREAD_PRIORITY_URGENT_AUDIO);
            } catch (Throwable ignored) {}
            try {
                while (alive) {
                    int rq = requestedQuality;
                    if (rq >= 0) {
                        requestedQuality = -1;
                        pcmQueue.clear();
                        out.write(Protocol.buildHello(sourceId, localName, level, codec, rq));
                        continue;
                    }
                    int requested = requestedLevel;
                    if (requested >= 0) {
                        requestedLevel = -1;
                        pcmQueue.clear();
                        reconfigure(codec, requested);
                        // The HELLO and following audio are written by the same thread.
                        // A receiver can never decode a new frame using the old format.
                        out.write(Protocol.buildHello(sourceId, localName, level, codec,
                                Settings.quality(ctx)));
                        continue;
                    }
                    // 控制包优先（HELLO / AUTH），保证它们及时发出
                    byte[] ctl = ctlQueue.poll();
                    if (ctl != null) { out.write(ctl); continue; }

                    short[] f = pcmQueue.poll(4, java.util.concurrent.TimeUnit.MILLISECONDS);
                    if (f == null) continue;
                    byte[] pkt = buildPacket(f);
                    if (pkt != null && auth == Protocol.AUTH_APPROVED && enabled) {
                        out.write(pkt);
                        totalBytesSent.addAndGet(pkt.length);
                    }
                }
            } catch (InterruptedException ignored) {
            } catch (Exception e) {
                alive = false;
            } finally {
            }
        }

        void requestLevel(int level) {
            requestedLevel = level;
        }

        void requestQuality(int quality) { requestedQuality = quality; }

        /** 持续读取对端消息：HELLO、STATS，以及反向剪贴板。 */
        private void readLoop() {
            byte[] buf = new byte[Protocol.CLIPBOARD_MAX_BYTES + Protocol.HDR_COMMON + 32];
            int fill = 0;
            try {
                while (alive) {
                    int r = in.read(buf, fill, buf.length - fill);
                    if (r <= 0) break;
                    fill += r;

                    while (fill >= Protocol.HDR_COMMON) {
                        if (!Protocol.validHeader(buf, fill)) { fill = 0; break; }
                        int need = Protocol.HDR_COMMON + Protocol.bodyLenOf(buf);
                        if (need <= 0 || need > buf.length) { fill = 0; break; }
                        if (fill < need) break;

                        byte type = Protocol.typeOf(buf);
                        if (type == Protocol.TYPE_HELLO) {
                            String n = Protocol.parseHelloName(buf, fill);
                            hasHello = true;
                            sendControl(Protocol.buildStreamState(enabled));
                            if (!n.isEmpty() && !n.equals(name)) {
                                name = n;
                                CaptureState.log("[发送] 接收端已接入：" + n
                                        + "（" + addr + " · " + link.kind() + "）");
                            }
                            if (auth == Protocol.AUTH_PENDING && !n.isEmpty()
                                    && Settings.isApproved(ctx, addr, n)) {
                                auth = Protocol.AUTH_APPROVED;
                                sendAuth();
                                CaptureState.log("[安全] " + n + " 是已批准设备，自动放行");
                            }
                        } else if (type == Protocol.TYPE_STATS) {
                            int ms = Protocol.parseStats(buf, fill);
                            if (ms >= 0) latencyMs = ms;
                        } else if (type == Protocol.TYPE_CLIPBOARD
                                && hasHello && auth == Protocol.AUTH_APPROVED) {
                            String text = Protocol.parseClipboard(buf, fill);
                            if (!text.isEmpty()) {
                                ClipboardManager cm = (ClipboardManager)
                                        ctx.getSystemService(Context.CLIPBOARD_SERVICE);
                                if (cm != null) {
                                    cm.setPrimaryClip(ClipData.newPlainText("AudioHub", text));
                                    ClipboardNotice.show(ctx, "已收到来自 " + name + " 的剪贴板内容");
                                    CaptureState.log("[剪贴板] 已接收来自 " + name + " 的内容（"
                                            + text.length() + " 字符）");
                                }
                            }
                        }
                        System.arraycopy(buf, need, buf, 0, fill - need);
                        fill -= need;
                    }
                    if (fill >= buf.length) fill = 0;
                }
            } catch (Exception ignored) {
            } finally {
                alive = false;
                writer.interrupt();
                link.close();
            }
        }

        void sendControl(byte[] p) {
            if (!alive || p == null) return;
            if (p[5] != Protocol.TYPE_CLIPBOARD) pcmQueue.clear();
            if (!ctlQueue.offer(p)) {
                ctlQueue.poll();
                ctlQueue.offer(p);
            }
        }

        void sendAuth() {
            try { sendControl(Protocol.buildAuth(sourceId, auth)); } catch (Exception ignored) {}
        }

        void setStreamEnabled(boolean value) {
            enabled = value;
            if (!value) pcmQueue.clear();
            if (hasHello) sendControl(Protocol.buildStreamState(value));
        }

        void sendClipboard(String text) {
            try { sendControl(Protocol.buildClipboard(text)); } catch (Exception ignored) {}
        }

        void setAuth(int status) {
            auth = status;
            sendAuth();
        }

        // ------------------------------------------------------------------
        /**
         * 追加采集到的 48kHz 立体声样本；攒够一帧就放进 PCM 队列。
         *
         * 采样率恒定为 48000、声道恒定为 2 —— 这是采集的原始格式，
         * 也是所有格式转换的统一入口。
         *
         * 【这里只做数组拷贝，不做编码】—— 编码交给 writeLoop，
         * 避免 MediaCodec 的阻塞拖垮采集节奏。
         */
        synchronized void pushPcm(short[] pcm, int len) {
            if (!alive || !enabled) return;
            // 安全：未批准的连接一个字节都不发。
            // 脚本能连上、能收到 HELLO，但拿不到任何音频。
            if (auth != Protocol.AUTH_APPROVED) return;
            int off = 0;
            while (off < len) {
                int space = need48 * 2 - accumFill;
                int take  = Math.min(space, len - off);
                System.arraycopy(pcm, off, accum, accumFill, take);
                accumFill += take;
                off += take;
                if (accumFill >= need48 * 2) {
                    short[] f = new short[need48 * 2];
                    System.arraycopy(accum, 0, f, 0, need48 * 2);
                    int maxQueued = 12;
                    while (pcmQueue.size() >= maxQueued) {
                        if (pcmQueue.poll() == null) break;
                        dropped.incrementAndGet();
                        totalFramesDropped.incrementAndGet();
                    }
                    if (!pcmQueue.offer(f)) {
                        dropped.incrementAndGet();
                        totalFramesDropped.incrementAndGet();
                    }
                    accumFill = 0;
                }
            }
        }

        /**
         * 把一帧 48kHz 立体声 PCM 转换成当前 WiFi 带宽档位，
         * 组装成完整的音频包。返回 null 表示本帧无数据可发。
         * 由 writeLoop 调用，【不在采集线程上】。
         */
        private byte[] buildPacket(short[] acc) {
            final int n48 = need48;
            byte[] payload;
            int payloadLen;
            int frameSamples;

            {
                int outSamples = Protocol.frameSamplesFor(Protocol.CODEC_PCM, level);
                int ch    = Protocol.bwChannels(level);
                int decim = Protocol.bwDecimation(level);
                if (decim == 1) {
                    System.arraycopy(acc, 0, convBuf, 0, outSamples * ch);
                } else if (ch == 2) {
                    // 立体声降采样：两个声道各自组内平均，【保持立体声】
                    for (int i = 0; i < outSamples; i++) {
                        int sl = 0, sr = 0;
                        for (int k = 0; k < decim; k++) {
                            int idx = (i * decim + k) * 2;
                            sl += acc[idx];
                            sr += acc[idx + 1];
                        }
                        convBuf[i * 2]     = (short) (sl / decim);
                        convBuf[i * 2 + 1] = (short) (sr / decim);
                    }
                } else {
                    // 单声道：左右相加后组内平均（兼作简单低通，抑制混叠）
                    for (int i = 0; i < outSamples; i++) {
                        int sum = 0;
                        for (int k = 0; k < decim; k++) {
                            int idx = (i * decim + k) * 2;
                            sum += (acc[idx] + acc[idx + 1]) >> 1;
                        }
                        convBuf[i] = (short) (sum / decim);
                    }
                }
                payloadLen   = outSamples * ch * 2;
                payload      = null;   // PCM 直接写入包体，不再多一次拷贝
                frameSamples = outSamples;
            }

            int total = Protocol.HDR_TOTAL + payloadLen;
            byte[] p = new byte[total];

            Protocol.writeHeader(p, Protocol.TYPE_AUDIO, sourceId,
                    Protocol.HDR_AUDIO + payloadLen);
            Protocol.putShortLE(p, Protocol.HDR_COMMON, sendSeq & 0xFFFF);
            Protocol.putShortLE(p, Protocol.HDR_COMMON + 2, frameSamples);
            Protocol.putIntLE(p, Protocol.HDR_COMMON + 4, (int) (System.nanoTime() / 1000L));

            if (payload != null) {
                System.arraycopy(payload, 0, p, Protocol.HDR_TOTAL, payloadLen);
            } else {
                int ch = Protocol.bwChannels(level);
                int outSamples = payloadLen / (ch * 2);
                int o = Protocol.HDR_TOTAL;
                for (int i = 0; i < outSamples * ch; i++) {
                    Protocol.putShortLE(p, o, convBuf[i]);
                    o += 2;
                }
            }

            sendSeq++;
            return p;
        }

        void close() {
            alive = false;
            writer.interrupt();
            reader.interrupt();
            link.close();
        }
    }

    // ==================================================================
    boolean start(int port, int sourceId, String deviceName, int bwLevel,
                  Context ctx, boolean approvalRequired) {
        if (running) return true;
        this.ctx = ctx;
        this.approvalRequired = approvalRequired;
        localName = (deviceName == null || deviceName.isEmpty()) ? "Android" : deviceName;
        this.bwLevel = Protocol.clampLevel(bwLevel);
        final int sid = sourceId;

        try {
            // ⚠️ 必须先创建未绑定的 ServerSocket、设置 SO_REUSEADDR、再 bind。
            //    `new ServerSocket(port)` 在构造时就已经 bind 了，之后再设
            //    setReuseAddress 完全无效；一旦端口处于 TIME_WAIT，
            //    重新监听就会 EADDRINUSE 直接失败。
            ServerSocket ss = new ServerSocket();
            ss.setReuseAddress(true);
            ss.bind(new InetSocketAddress(port));
            server = ss;
        } catch (Exception e) {
            CaptureState.log("[网络] 监听 TCP " + port + " 失败: " + e);
            return false;
        }
        running = true;
        CaptureState.log("[网络] 已开始广播，端口 " + port + "，本机地址 " + localAddresses()
                + "，带宽档位 " + Protocol.bwLabel(this.bwLevel)
                + "（" + Protocol.bwKbps(this.bwLevel) + " kbps）");

        acceptThread = new Thread(() -> acceptLoop(sid), "ahub-accept");
        acceptThread.start();


        authRunning = true;
        authThread = new Thread(this::authLoop, "ahub-auth");
        authThread.start();
        return true;
    }

    void refreshQuality() {
        if (!running || ctx == null) return;
        int q = Settings.quality(ctx);
        for (Client c : clients) if (c.alive && c.hasHello) c.requestQuality(q);
        CaptureState.log("[延迟] 已通知接收端切换到 " + Settings.qualityLabel(ctx));
    }

    void stop() {
        if (!running) return;
        running = false;
        authRunning = false;
        if (authThread != null) authThread.interrupt();
        for (Client c : clients) c.close();
        clients.clear();
        try { if (server != null) server.close(); } catch (Exception ignored) {}
        try { if (acceptThread != null) acceptThread.join(800); } catch (InterruptedException ignored) {}
        try { if (authThread != null) authThread.join(500); } catch (InterruptedException ignored) {}
        server = null;
        CaptureState.log("[网络] 已停止广播");
    }

    /**
     * 周期性维护：清理死连接、清理未握手的扫描探测、同地址只保留最新连接、
     * 以及把"已记住的设备"放行。
     *
     * 放在单独线程而不是 accept 时判断：accept 时新连接还没发 HELLO，
     * 无法与扫描探测区分 —— 贸然踢旧连接会造成"一卡一卡、设备来回掉线"。
     */
    private void authLoop() {
        while (authRunning) {
            try { Thread.sleep(clients.isEmpty() ? 5000 : 1000); }
            catch (InterruptedException e) { return; }

            long now = System.currentTimeMillis();
            for (Client c : clients) {
                if (!c.alive) { clients.remove(c); continue; }
                if (!c.hasHello && now - c.createdMs > 3000) {
                    c.close();
                    clients.remove(c);
                }
            }

            for (Client a : clients) {
                if (!a.alive || !a.hasHello) continue;
                for (Client b : clients) {
                    if (b != a && b.alive && b.hasHello
                            && b.addr.equals(a.addr) && b.createdMs < a.createdMs) {
                        b.close();
                        clients.remove(b);
                        CaptureState.log("[网络] " + a.addr + " 重连，已替换旧连接");
                    }
                }
            }

            if (ctx == null) continue;


            for (Client c : clients) {
                if (!c.alive || c.auth != Protocol.AUTH_PENDING) continue;
                String n = c.name;
                if (n == null || n.isEmpty()) continue;
                if (Settings.isApproved(ctx, c.addr, n)) {
                    c.setAuth(Protocol.AUTH_APPROVED);
                    CaptureState.log("[安全] " + n + " 在已批准列表中，自动放行");
                }
            }
        }
    }

    boolean isRunning() { return running; }
    long framesSent()    { return framesSent.get(); }
    long framesDropped() { return framesDropped.get(); }
    long bytesSent()     { return bytesSent.get(); }

    int clientCount() {
        int n = 0;
        for (Client c : clients) if (c.alive && c.hasHello) n++;
        return n;
    }

    /** 已连接且【已批准】的接收端数量（决定是否继续采集、是否耗电） */
    int approvedCount() {
        int n = 0;
        for (Client c : clients) {
            if (c.alive && c.hasHello && c.auth == Protocol.AUTH_APPROVED) n++;
        }
        return n;
    }

    int enabledCount() {
        int n = 0;
        for (Client c : clients) if (c.alive && c.hasHello && c.enabled) n++;
        return n;
    }

    /** 界面上显示用的列表：按地址去重，未握手的扫描探测不显示 */
    List<Client> clientList() {
        java.util.Map<String, Client> newest = new java.util.HashMap<>();
        for (Client c : clients) {
            if (!c.alive || !c.hasHello) continue;
            Client prev = newest.get(c.addr);
            if (prev == null || c.createdMs > prev.createdMs) newest.put(c.addr, c);
        }
        List<Client> out = new ArrayList<>(newest.values());
        Collections.sort(out, (a, b) -> a.name.compareToIgnoreCase(b.name));
        return out;
    }

    boolean setClientAuth(String addr, int status) {
        for (Client c : clients) {
            if (c.alive && c.addr.equals(addr)) { c.setAuth(status); return true; }
        }
        return false;
    }

    boolean disconnect(String addr) {
        for (Client c : clients) {
            if (c.addr.equals(addr)) { c.close(); clients.remove(c); return true; }
        }
        return false;
    }

    /** 撤销所有已连接接收端的授权（设置里"全部撤销"时调用） */
    void revokeAll() {
        int n = 0;
        for (Client c : clients) {
            if (c.alive && c.auth == Protocol.AUTH_APPROVED) {
                c.setAuth(Protocol.AUTH_PENDING);
                n++;
            }
        }
        CaptureState.log(n > 0
                ? "[安全] 已撤销 " + n + " 个接收端的授权，音频已停止发送"
                : "[安全] 已清空已批准设备列表");
    }

    /**
     * 运行中切换 WiFi 音频带宽档位。
     * 【不重启监听】，只给每个客户端补发 HELLO 让它重建缓冲。
     */
    void setBandwidth(int level) {
        this.bwLevel = Protocol.clampLevel(level);
        int n = 0;
        for (Client c : clients) {
            if (!c.alive || !c.hasHello) continue;
            c.requestLevel(this.bwLevel);
            n++;
        }
        CaptureState.log("[带宽] 已通知 " + n + " 个接收端切换到 "
                + Protocol.bwLabel(this.bwLevel)
                + "（" + Protocol.bwKbps(this.bwLevel) + " kbps），连接未断开");
    }

    /**
     * 把一帧采集到的 48kHz 立体声 PCM 分发给所有已批准的接收端。
     * 每个客户端按自己的编码方式与档位自行转换。
     */
    void sendFrame(short[] pcm48Stereo, int len) {
        if (!running || pcm48Stereo == null || len <= 0) return;
        boolean any = false;
        for (Client c : clients) {
            if (!c.alive) { clients.remove(c); continue; }
            if (!c.hasHello) continue;
            if (!c.enabled) continue;
            if (c.auth != Protocol.AUTH_APPROVED) continue;
            c.pushPcm(pcm48Stereo, len);
            any = true;
        }
        if (any) {
            framesSent.incrementAndGet();
        }
    }

    /** 向当前已批准的接收端发送本机剪贴板。 */
    int sendClipboard(String text) {
        int n = 0;
        if (text == null) text = "";
        for (Client c : clients) {
            if (!c.alive || !c.hasHello || c.auth != Protocol.AUTH_APPROVED) continue;
            c.sendClipboard(text);
            n++;
        }
        CaptureState.log("[剪贴板] 已发送到 " + n + " 个接收端");
        return n;
    }

    /** 本机局域网地址 */
    static String localAddresses() {
        StringBuilder sb = new StringBuilder();
        for (String ip : LanScan.localIPv4()) {
            if (sb.length() > 0) sb.append(", ");
            sb.append(ip);
        }
        return sb.length() == 0 ? "未知" : sb.toString();
    }

    // ------------------------------------------------------------------
    private void acceptLoop(int sourceId) {
        while (running) {
            try {
                Socket s = server.accept();
                s.setTcpNoDelay(true);
                s.setSendBufferSize(256 * 1024);
                Client c = new Client(new AudioLink.Tcp(s), sourceId, localName,
                        bwLevel, ctx, approvalRequired, bytesSent, framesDropped);
                // 这里【不做】同地址替换：此刻新连接还没发 HELLO，
                // 无法区分真实接收端与扫描探测。替换交给 authLoop。
                clients.add(c);
            } catch (Exception e) {
                if (running) CaptureState.log("[网络] accept 异常: " + e);
                if (!running) break;
            }
        }
    }
}
