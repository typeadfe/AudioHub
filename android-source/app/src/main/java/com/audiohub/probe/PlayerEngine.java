package com.audiohub.probe;

import android.content.Context;
import android.media.AudioAttributes;
import android.media.AudioFocusRequest;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.os.Build;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.concurrent.CopyOnWriteArrayList;

/**
 * 接收端引擎：多源 TCP 接入 + 抖动缓冲 + 混音 + AudioTrack 低延迟输出。
 *
 * 三个线程：
 *   - 扫描线程：仅在用户点击扫描按钮时探测局域网
 *   - 每源一个网络线程（在 PlayerSource 内）
 *   - 渲染线程：按 5ms 一帧拉取各路数据 → 求和 → 主音量 → 软限幅 → 写入 AudioTrack
 *
 * 输出用 AudioTrack + PERFORMANCE_MODE_LOW_LATENCY，纯 Java 实现，不需要 NDK。
 */
final class PlayerEngine {

    interface Listener { void onPlayerChanged(); }

    private final Context ctx;
    private final List<PlayerSource> sources = new CopyOnWriteArrayList<>();

    private AudioTrack track;
    private Thread renderThread;
    private Thread scanThread;
    private ServerSocket reverseServer;
    private Thread reverseThread;
    private final Object scanWake = new Object();
    private boolean refreshPending = false;
    /** 广播本机存在，让对方的『发送』界面能发现本机 */
    private AnnounceSender announcer;
    private volatile boolean running = false;

    volatile float  masterGain = 1.0f;
    volatile String status = "未启动";
    volatile int    deviceBufferMs = 0;
    volatile long   lastScanMs = 0;
    volatile int    lastScanFound = 0;

    private volatile Listener listener;

    // 独占音频
    private AudioManager audioManager;
    private AudioFocusRequest focusRequest;
    private boolean holdingFocus = false;

    PlayerEngine(Context ctx) {
        this.ctx = ctx.getApplicationContext();
    }

    void setListener(Listener l) { listener = l; }

    private void notifyChanged() {
        Listener l = listener;
        if (l != null) { try { l.onPlayerChanged(); } catch (Exception ignored) {} }
    }

    boolean isRunning() { return running; }

    List<PlayerSource> sources() { return new ArrayList<>(sources); }

    /** 立即执行一次局域网扫描；用于界面的「刷新」按钮。 */
    void refreshNow() {
        if (!running) return;
        synchronized (scanWake) {
            refreshPending = true;
            scanWake.notifyAll();
        }
    }

    /** 向当前已连接的音源发送本机剪贴板。 */
    int sendClipboard(String text) {
        int n = 0;
        for (PlayerSource s : sources) {
            if (s.enabled && s.isConnected() && s.sendClipboard(text)) n++;
        }
        CaptureState.log("[剪贴板] 已发送到 " + n + " 个发送端");
        return n;
    }

    int connectedCount() {
        int c = 0;
        for (PlayerSource s : sources) if (s.enabled && s.isConnected()) c++;
        return c;
    }

    // ------------------------------------------------------------------
    boolean start() {
        if (running) return true;
        if (!createTrack()) return false;

        // 应用设置里的缓冲档位
        PlayerJitterBuffer.setTargetMs(Settings.jitterTargetMs(ctx));
        CaptureState.log("[接收] 缓冲目标 " + PlayerJitterBuffer.targetMs() + "ms（"
                + Settings.qualityLabel(ctx) + "）");

        applyAudioFocus();

        running = true;
        status = "运行中";
        track.play();

        renderThread = new Thread(this::renderLoop, "ahub-render");
        renderThread.start();
        scanThread = new Thread(this::scanLoop, "ahub-scan");
        scanThread.start();
        startReverseListener();

        // 广播"本机是可用接收端"。
        // 连接方向是"接收端主动连发送端"，所以不广播的话，
        // 对方的『发送』界面上永远看不到本机。
        announcer = new AnnounceSender(Settings.deviceName(ctx));
        announcer.start();

        CaptureState.log("[接收] 已启动，输出 " + Protocol.PLAY_SAMPLE_RATE + " Hz 立体声，"
                + (Settings.exclusiveAudio(ctx) ? "独占音频" : "与本机其他声音同时播放"));
        return true;
    }

    void stop() {
        if (!running) return;
        running = false;
        synchronized (scanWake) { scanWake.notifyAll(); }
        if (announcer != null) { announcer.stop(); announcer = null; }
        try { if (reverseServer != null) reverseServer.close(); } catch (Exception ignored) {}
        try { if (reverseThread != null) reverseThread.join(700); } catch (InterruptedException ignored) {}
        reverseServer = null;
        reverseThread = null;
        try { if (track != null) track.stop(); } catch (Exception ignored) {}
        try { if (renderThread != null) renderThread.join(1000); } catch (InterruptedException ignored) {}
        try { if (scanThread != null) scanThread.join(1500); } catch (InterruptedException ignored) {}
        for (PlayerSource s : sources) s.stop();
        sources.clear();
        try { if (track != null) track.release(); } catch (Exception ignored) {}
        track = null;
        renderThread = null;
        scanThread = null;
        abandonAudioFocus();
        status = "已停止";
        CaptureState.log("[接收] 已停止");
        notifyChanged();
    }

    /** 电脑主动连入手机，避开 Windows 入站防火墙。 */
    private void startReverseListener() {
        try {
            ServerSocket ss = new ServerSocket();
            ss.setReuseAddress(true);
            ss.bind(new InetSocketAddress(Protocol.RECEIVER_PORT));
            reverseServer = ss;
            reverseThread = new Thread(() -> {
                while (running) {
                    Socket socket;
                    try { socket = ss.accept(); }
                    catch (Exception e) { if (running) CaptureState.log("[接收] 反向连接异常: " + e); break; }
                    String host = socket.getInetAddress().getHostAddress();
                    for (PlayerSource old : sources) {
                        if (!old.host.equals(host)) continue;
                        if (old.isConnected()) {
                            try { socket.close(); } catch (Exception ignored) {}
                            socket = null;
                            break;
                        }
                        old.stop();
                        sources.remove(old);
                    }
                    if (socket == null) continue;
                    PlayerSource source = new PlayerSource(socket, ctx,
                            Settings.deviceName(ctx), Settings.bandwidth(ctx));
                    source.setOnClosed(() -> {
                        sources.remove(source);
                        notifyChanged();
                    });
                    sources.add(source);
                    source.start();
                    notifyChanged();
                    CaptureState.log("[接收] 电脑发送端主动接入 " + host);
                }
            }, "ahub-reverse-listener");
            reverseThread.start();
            CaptureState.log("[接收] 已监听电脑主动连接，端口 " + Protocol.RECEIVER_PORT);
        } catch (Exception e) {
            CaptureState.log("[接收] 无法监听电脑主动连接: " + e);
        }
    }

    // ------------------------------------------------------------------
    /** 设置里勾选了「独占音频」才请求焦点；否则不请求，天然与其他应用混音 */
    private void applyAudioFocus() {
        if (!Settings.exclusiveAudio(ctx)) return;
        try {
            audioManager = (AudioManager) ctx.getSystemService(Context.AUDIO_SERVICE);
            if (audioManager == null) return;
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                focusRequest = new AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
                        .setAudioAttributes(new AudioAttributes.Builder()
                                .setUsage(AudioAttributes.USAGE_MEDIA)
                                .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                                .build())
                        .setOnAudioFocusChangeListener(change -> { })
                        .build();
                holdingFocus = audioManager.requestAudioFocus(focusRequest)
                        == AudioManager.AUDIOFOCUS_REQUEST_GRANTED;
            } else {
                holdingFocus = audioManager.requestAudioFocus(null,
                        AudioManager.STREAM_MUSIC, AudioManager.AUDIOFOCUS_GAIN)
                        == AudioManager.AUDIOFOCUS_REQUEST_GRANTED;
            }
            CaptureState.log("[接收] 独占音频焦点: " + (holdingFocus ? "已获得" : "被拒绝"));
        } catch (Throwable t) {
            CaptureState.log("[接收] 请求音频焦点失败: " + t);
        }
    }

    private void abandonAudioFocus() {
        if (!holdingFocus) return;
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O && focusRequest != null) {
                audioManager.abandonAudioFocusRequest(focusRequest);
            } else if (audioManager != null) {
                audioManager.abandonAudioFocus(null);
            }
        } catch (Throwable ignored) {}
        holdingFocus = false;
    }

    // ------------------------------------------------------------------
    private boolean createTrack() {
        try {
            int minBuf = AudioTrack.getMinBufferSize(
                    Protocol.PLAY_SAMPLE_RATE,
                    AudioFormat.CHANNEL_OUT_STEREO,
                    AudioFormat.ENCODING_PCM_16BIT);
            if (minBuf <= 0) minBuf = 1920 * 4;
            int bufBytes = minBuf * 2;

            track = new AudioTrack.Builder()
                    .setAudioAttributes(new AudioAttributes.Builder()
                            .setUsage(AudioAttributes.USAGE_MEDIA)
                            .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                            .build())
                    .setAudioFormat(new AudioFormat.Builder()
                            .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                            .setSampleRate(Protocol.PLAY_SAMPLE_RATE)
                            .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
                            .build())
                    .setBufferSizeInBytes(bufBytes)
                    .setTransferMode(AudioTrack.MODE_STREAM)
                    .setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY)
                    .build();

            if (track.getState() != AudioTrack.STATE_INITIALIZED) {
                CaptureState.log("[接收] AudioTrack 初始化失败，state=" + track.getState());
                track.release();
                track = null;
                return false;
            }
            deviceBufferMs = (int) (bufBytes * 1000L / (Protocol.PLAY_SAMPLE_RATE * 4));
            PlayerSource.outputBufferMs = deviceBufferMs;   // 供延迟估算使用
            CaptureState.log("[接收] 输出缓冲约 " + deviceBufferMs + "ms");
            return true;
        } catch (Throwable t) {
            CaptureState.log("[接收] 创建 AudioTrack 异常: " + t);
            return false;
        }
    }

    // ------------------------------------------------------------------
    private void scanLoop() {
        while (running) {
            synchronized (scanWake) {
                while (running && !refreshPending) {
                    try { scanWake.wait(); } catch (InterruptedException ignored) {}
                }
                if (!running) break;
                refreshPending = false;
            }
            try {
                List<String> myIps = LanScan.localIPv4();
                List<String> found = LanScan.scan(Protocol.DISCOVERY_PORT, 250, () -> running);
                lastScanMs = System.currentTimeMillis();
                lastScanFound = found.size();
                for (String h : found) {
                    if (myIps.contains(h) || !running) continue;
                    addDiscoveredSource(h);
                }
            } catch (Throwable t) {
                CaptureState.log("[接收] 扫描异常: " + t);
            }
        }
    }

    private void addDiscoveredSource(String host) {
        if (!running || host == null || host.isEmpty()
                || LanScan.localIPv4().contains(host)) return;
        PlayerSource source;
        synchronized (sources) {
            if (hasSource(host)) return;
            source = new PlayerSource(host, Protocol.DISCOVERY_PORT,
                    ctx, Settings.deviceName(ctx), Settings.bandwidth(ctx));
            source.enabled = Settings.autoConnect(ctx);
            sources.add(source);
        }
        if (source.enabled) source.start();
        CaptureState.log("[接收] 发现设备 " + host
                + (source.enabled ? "，已自动连接" : "，等待你勾选"));
        notifyChanged();
    }
    private boolean hasSource(String host) {
        for (PlayerSource s : sources) if (s.host.equals(host)) return true;
        return false;
    }

    /** 用户勾选/取消勾选某一路 */
    void setSourceEnabled(PlayerSource s, boolean enabled) {
        s.enabled = enabled;
        if (enabled) {
            s.start();
            CaptureState.log("[接收] 已连接 " + s.displayName());
        } else {
            s.stop();
            CaptureState.log("[接收] 已断开 " + s.displayName());
        }
        notifyChanged();
    }

    // ------------------------------------------------------------------
    private void renderLoop() {
        // 渲染线程直接决定声音是否连续，被系统抢占就会听到断续/爆音
        try {
            android.os.Process.setThreadPriority(
                    android.os.Process.THREAD_PRIORITY_URGENT_AUDIO);
        } catch (Throwable ignored) {}

        final int samples = Protocol.FRAME_SAMPLES * Protocol.CHANNELS;
        final float[] acc = new float[samples];
        final short[] out = new short[samples];
        boolean outputPaused = false;

        while (running) {
            // 省电：没有任何活跃音源时暂停 AudioTrack，避免空转
            boolean anyLive = false;
            for (PlayerSource s : sources) {
                if (s.enabled && s.isActive()) { anyLive = true; break; }
            }
            if (!anyLive) {
                if (!outputPaused) {
                    try { track.pause(); } catch (Exception ignored) {}
                    try { track.flush(); } catch (Exception ignored) {}
                    outputPaused = true;
                }
                try { Thread.sleep(500); } catch (InterruptedException e) { break; }
                continue;
            }
            if (outputPaused) {
                try { track.play(); } catch (Exception ignored) {}
                outputPaused = false;
            }

            Arrays.fill(acc, 0f);
            for (PlayerSource s : sources) {
                if (!s.enabled) continue;
                try { s.mixInto(acc, Protocol.FRAME_SAMPLES); } catch (Throwable ignored) {}
            }

            final float mg = masterGain;
            for (int i = 0; i < samples; i++) out[i] = softLimit(acc[i] * mg);

            AudioTrack t = track;
            if (t == null) break;
            try {
                t.write(out, 0, samples);   // MODE_STREAM 下会阻塞，正好给循环定节拍
            } catch (Throwable e) {
                break;
            }
        }
    }

    /** 软限幅：拐点 0.95 满刻度，单源几乎不被压缩，只有多源叠加超出时才介入 */
    private static short softLimit(float x) {
        final float T    = 0.95f * 32767f;
        final float room = 32767f - T;
        float f = x;
        if (f > T || f < -T) {
            float sgn  = (f > 0f) ? 1f : -1f;
            float a    = Math.abs(f);
            float over = a - T;
            a = T + room * (over / (over + room));
            f = sgn * a;
        }
        if (f >  32767f) f =  32767f;
        if (f < -32768f) f = -32768f;
        return (short) f;
    }
}
