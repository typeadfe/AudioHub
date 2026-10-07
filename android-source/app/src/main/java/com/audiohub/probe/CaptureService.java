package com.audiohub.probe;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioPlaybackCaptureConfiguration;
import android.media.AudioRecord;
import android.media.projection.MediaProjection;
import android.media.projection.MediaProjectionManager;
import android.os.Build;
import android.os.IBinder;
import android.os.SystemClock;
import android.util.Log;

import java.io.File;
import java.util.Locale;

/**
 * 前台服务：用 MediaProjection + AudioPlaybackCapture 采集系统音频，
 * 主界面可见时统计电平，并把 PCM 写成 WAV。
 *
 * 判定逻辑：采集不到目标应用声音时系统【不会报错】，只会读到全 0 的静音数据。
 * 所以本服务以「是否读到非静音样本」作为唯一判据，并把它暴露给界面。
 */
public class CaptureService extends Service {

    private static final String TAG = "AudioHubProbe";

    static final String ACTION_START = "com.audiohub.probe.START";
    static final String ACTION_STOP = "com.audiohub.probe.STOP";
    static final String ACTION_CLIPBOARD_START = "com.audiohub.probe.CLIPBOARD_START";
    static final String EXTRA_RESULT_CODE = "resultCode";
    static final String EXTRA_RESULT_DATA = "resultData";

    private static final int SAMPLE_RATE = 48000;
    private static final int CHANNEL_MASK = AudioFormat.CHANNEL_IN_STEREO;
    private static final int ENCODING = AudioFormat.ENCODING_PCM_16BIT;
    private static final int CHANNELS = 2;
    private static final int BITS = 16;

    /** 每次读取一帧 5ms 的数据：48000 * 2ch * 2B * 0.005s = 960 字节 */
    private static final int READ_CHUNK = Protocol.FRAME_BYTES;

    private static final int NOTIF_ID = 0x4155;
    private static final String CHANNEL_ID = "audiohub_capture";

    private MediaProjection projection;
    private MediaProjection.Callback projectionCallback;
    private AudioRecord audioRecord;
    private Thread readerThread;
    private volatile boolean stopping = false;

    // ---- WiFi 音频服务端（由接收端主动连入） ----
    private AudioServer server;
    /** 当前 WiFi 音频带宽档位 */
    private int bwLevel = Protocol.BW_FULL;
    /** 48kHz 立体声采集缓冲（short，交错）。这是所有格式转换的统一入口 */
    private short[] pcm48 = new short[Protocol.FRAME_SAMPLES * Protocol.CHANNELS];

    /** 供界面读取"已连接的接收端"列表 */
    private static volatile AudioServer activeServer;
    static AudioServer activeServer() { return activeServer; }

    // ---- 接收端存在广播的监听（让发送端能列出尚未连接的接收端） ----
    private AnnounceListener announce;
    private static volatile AnnounceListener activeAnnounce;
    static AnnounceListener activeAnnounce() { return activeAnnounce; }
    private int sourceId = 0;

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        Settings.applyRuntime(this);   // 日志开关、缓冲档位
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent == null) {
            stopSelf();
            return START_NOT_STICKY;
        }
        String action = intent.getAction();
        if (ACTION_STOP.equals(action)) {
            CaptureState.log("[服务] 收到停止指令");
            shutdown();
            return START_NOT_STICKY;
        }
        if (ACTION_START.equals(action)) {
            int resultCode = intent.getIntExtra(EXTRA_RESULT_CODE, 0);
            Intent resultData = intent.getParcelableExtra(EXTRA_RESULT_DATA);
            startCapture(resultCode, resultData);
        } else if (ACTION_CLIPBOARD_START.equals(action)) {
            startClipboardOnly();
        }
        return START_NOT_STICKY;
    }

    // ------------------------------------------------------------------
    // 启动采集
    // ------------------------------------------------------------------
    private void startCapture(int resultCode, Intent resultData) {
        CaptureState.reset();
        CaptureState.running = true;

        // 1) 必须先在 5 秒内进入前台，且 Android 14+ 要求声明 mediaProjection 类型
        startForegroundCompat();

        // 2) 取得 MediaProjection
        MediaProjectionManager mpm =
                (MediaProjectionManager) getSystemService(MEDIA_PROJECTION_SERVICE);
        if (mpm == null) {
            fail("拿不到 MediaProjectionManager");
            return;
        }
        try {
            projection = mpm.getMediaProjection(resultCode, resultData);
        } catch (Throwable t) {
            fail("getMediaProjection 抛异常: " + t);
            return;
        }
        if (projection == null) {
            fail("MediaProjection 为 null（授权被拒绝或令牌无效）");
            return;
        }
        CaptureState.log("[投影] 取得 MediaProjection 成功");

        // 3) 注册回调（Android 14+ 强制要求）
        projectionCallback = new MediaProjection.Callback() {
            @Override
            public void onStop() {
                CaptureState.log("[投影] 系统通知投影已停止");
                shutdown();
            }
        };
        try {
            projection.registerCallback(projectionCallback, null);
        } catch (Throwable t) {
            CaptureState.log("[投影] registerCallback 失败: " + t);
        }

        // 4) 构造采集配置：尽量覆盖各类用途
        AudioPlaybackCaptureConfiguration config;
        try {
            AudioPlaybackCaptureConfiguration.Builder b =
                    new AudioPlaybackCaptureConfiguration.Builder(projection);
            b.addMatchingUsage(AudioAttributes.USAGE_MEDIA);
            b.addMatchingUsage(AudioAttributes.USAGE_GAME);
            b.addMatchingUsage(AudioAttributes.USAGE_UNKNOWN);
            tryAddUsage(b, AudioAttributes.USAGE_ALARM);
            tryAddUsage(b, AudioAttributes.USAGE_NOTIFICATION);
            tryAddUsage(b, AudioAttributes.USAGE_NOTIFICATION_RINGTONE);
            config = b.build();
            CaptureState.log("[配置] 采集用途: MEDIA/GAME/UNKNOWN/ALARM/NOTIFICATION");
        } catch (Throwable t) {
            fail("构造采集配置失败: " + t);
            return;
        }

        // 6) 构造 AudioRecord
        AudioFormat format = new AudioFormat.Builder()
                .setEncoding(ENCODING)
                .setSampleRate(SAMPLE_RATE)
                .setChannelMask(CHANNEL_MASK)
                .build();

        int minBuf = AudioRecord.getMinBufferSize(SAMPLE_RATE, CHANNEL_MASK, ENCODING);
        int bufBytes = Math.max(minBuf > 0 ? minBuf * 4 : 0, READ_CHUNK * 8);
        CaptureState.log("[配置] 48kHz / 立体声 / PCM16，minBuf=" + minBuf + "，用 buf=" + bufBytes);

        try {
            audioRecord = new AudioRecord.Builder()
                    .setAudioFormat(format)
                    .setBufferSizeInBytes(bufBytes)
                    .setAudioPlaybackCaptureConfig(config)
                    .build();
        } catch (Throwable t) {
            fail("构造 AudioRecord 失败: " + t);
            return;
        }

        CaptureState.recordState = audioRecord.getState();
        CaptureState.log("[采集] AudioRecord.getState() = " + CaptureState.recordState
                + "（1 表示 STATE_INITIALIZED 正常）");
        if (audioRecord.getState() != AudioRecord.STATE_INITIALIZED) {
            fail("AudioRecord 未初始化成功（state=" + CaptureState.recordState + "）");
            return;
        }

        // 7) 开始读取
        stopping = false;
        try {
            audioRecord.startRecording();
        } catch (Throwable t) {
            fail("startRecording 失败: " + t);
            return;
        }
        CaptureState.log("[采集] 已开始读取，请打开你的游戏/音乐");

        // 9) 启动音频服务端（接收端会主动连进来）
        startNetworkServer();
        readerThread = new Thread(this::readLoop, "audiohub-reader");
        readerThread.start();
    }

    /** 只启动 WiFi 控制/剪贴板服务，不请求录屏权限，也不采集声音。 */
    private void startClipboardOnly() {
        if (server != null && server.isRunning()) return;
        stopping = false;
        CaptureState.reset();
        startForegroundCompat(false);
        startNetworkServer();
        CaptureState.log("[剪贴板] 已启动网络服务；接收端刷新后即可连接");
    }

    /** 启动共享 TCP 服务端。音频发送与剪贴板发送共用同一条连接。 */
    private void startNetworkServer() {
        if (server != null && server.isRunning()) return;
        sourceId = new java.util.Random().nextInt();
        if (sourceId == 0) sourceId = 1;
        bwLevel = Settings.bandwidth(this);
        CaptureState.log("[带宽] WiFi 路径： " + Protocol.bwLabel(bwLevel) + " · "
                + Protocol.bwFormatText(bwLevel) + " · " + Protocol.bwKbps(bwLevel) + " kbps");

        server = new AudioServer();
        if (!server.start(Protocol.DISCOVERY_PORT, sourceId, Settings.deviceName(this), bwLevel,
                getApplicationContext(), Settings.approvalRequired(this))) {
            CaptureState.log("[网络] TCP 服务启动失败");
            server = null;
        }
        activeServer = server;
        if (server != null) {
            CaptureState.netInfo = String.format(Locale.US,
                    "广播中 · %s\n本机地址 %s  端口 %d",
                    Settings.deviceName(this), AudioServer.localAddresses(),
                    Protocol.DISCOVERY_PORT);
        }
        if (server != null && Settings.approvalRequired(this)) {
            CaptureState.log("[安全] 已开启连接审批：新设备接入后需你在本页点【允许】才会开始发送");
        }
        if (announce == null) {
            announce = new AnnounceListener();
            announce.start();
            activeAnnounce = announce;
        }
    }

    /** 向已批准的接收端发送剪贴板文本。 */
    static int sendClipboard(String text) {
        AudioServer s = activeServer;
        return s == null ? -1 : s.sendClipboard(text == null ? "" : text);
    }

    /** 逐个尝试添加用途，个别用途不被允许时忽略即可 */
    private void tryAddUsage(AudioPlaybackCaptureConfiguration.Builder b, int usage) {
        try {
            b.addMatchingUsage(usage);
        } catch (Throwable ignored) {
        }
    }

    // ------------------------------------------------------------------
    // 读取循环
    // ------------------------------------------------------------------
    private void readLoop() {
        // 采集线程一旦被抢占，AudioRecord 的内部缓冲就会溢出 → 直接丢数据 → 咔哒声。
        
        try {
            android.os.Process.setThreadPriority(
                    android.os.Process.THREAD_PRIORITY_URGENT_AUDIO);
        } catch (Throwable ignored) {}

        byte[] buf = new byte[READ_CHUNK];
        long lastLog = 0;
        long idleSince = 0;
        boolean paused = false;

        while (!stopping) {
            // ---- 后台省电：应用在后台且没有任何接收端连接时，暂停采集 ----
            // 采集（AudioRecord + MediaProjection）是发送端最耗电的部分，
            // 没人接收时继续采集纯属浪费。有接收端接入会立刻自动恢复。
            boolean noClients = (server == null || server.approvedCount() == 0);
            if (noClients) CaptureState.connectedSinceMs = 0;
            else if (CaptureState.connectedSinceMs == 0)
                CaptureState.connectedSinceMs = SystemClock.elapsedRealtime();
            if (!AppState.foreground && noClients) {
                if (idleSince == 0) idleSince = System.currentTimeMillis();
                if (!paused && System.currentTimeMillis() - idleSince > 10000) {
                    pauseCapture();
                    paused = true;
                }
            } else {
                idleSince = 0;
                if (paused) { resumeCapture(); paused = false; }
            }
            if (paused) {
                try { Thread.sleep(300); } catch (InterruptedException e) { break; }
                continue;
            }

            int n;
            try {
                n = audioRecord.read(buf, 0, buf.length);
            } catch (Throwable t) {
                CaptureState.log("[采集] read 抛异常: " + t);
                break;
            }
            if (n <= 0) {
                CaptureState.log("[采集] read 返回 " + n + "，结束");
                break;
            }

            // ---- 本机输出增益 ----
            // 在发送前生效，因此电平表 / WAV / 推流三者一致：
            // 界面上看到多少，电脑收到的就是多少。
            final float gain = CaptureState.inputGain;
            if (gain != 1.0f) {
                if (gain <= 0.0001f) {
                    java.util.Arrays.fill(buf, 0, n, (byte) 0);
                } else {
                    for (int i = 0; i + 1 < n; i += 2) {
                        int lo = buf[i] & 0xFF;
                        int hi = buf[i + 1];
                        int s = (hi << 8) | lo;          // 有符号 16bit 小端
                        if (s > 32767) s -= 65536;
                        int v = (int) (s * gain);
                        if (v > 32767) v = 32767;
                        else if (v < -32768) v = -32768;
                        buf[i] = (byte) (v & 0xFF);
                        buf[i + 1] = (byte) ((v >> 8) & 0xFF);
                    }
                }
            }

            // ---- 带宽设置变化：立即生效 ----
            // 早期版本只在「开始发送」时读一次设置，运行中改档位完全不起作用。
            // 现在检测到变化就重启广播，接收端会自动重连并拿到新格式。
            {
                int want = Settings.bandwidth(this);
                if (want != bwLevel) {
                    CaptureState.log("[带宽] 切换为 " + Protocol.bwLabel(want)
                            + "（" + Protocol.bwFormatText(want) + "，"
                            + Protocol.bwKbps(want) + " kbps）");
                    applyBandwidth(want);
                }
            }

            // ---- 转成 48kHz 立体声 short 后交给服务端分发 ----
            // AudioRecord 固定按 48kHz 立体声读，每次读恰好一帧（5ms = 240 样本/声道）。
            // 各接收端的具体格式（采样率 / 声道 / 编码）由服务端按连接分别处理，
            // 所有接收端都通过 WiFi 接收 PCM，具体带宽档位由发送端统一决定。
            if (server != null && server.isRunning()
                    && n >= Protocol.FRAME_BYTES && server.approvedCount() > 0) {
                toPcm48(buf, n);
                server.sendFrame(pcm48, n / 2);
            }

            CaptureState.bytesTotal += n;
            // The service keeps transmitting in the background. Meter analysis is
            // only useful while the main screen is visible.
            if (AppState.metersVisible) {
                long sumSq = 0;
                int peak = 0;
                int samples = n / 2;
                for (int i = 0; i < samples; i++) {
                    int lo = buf[i * 2] & 0xFF;
                    int hi = buf[i * 2 + 1];
                    int s = (hi << 8) | lo;
                    if (s > 32767) s -= 65536;
                    int a = Math.abs(s);
                    if (a > peak) peak = a;
                    sumSq += (long) s * s;
                }

                double rms = samples > 0 ? Math.sqrt((double) sumSq / samples) : 0;
                CaptureState.rmsDb = rms > 0
                        ? (float) (20 * Math.log10(rms / 32768.0)) : -120f;
                int pct = (int) Math.round(rms / 32768.0 * 400);
                CaptureState.levelPercent = Math.min(pct, 100);
                if (peak > CaptureState.peakAbs) CaptureState.peakAbs = peak;
                if (peak > 32) {
                    CaptureState.bytesSignal += n;
                    CaptureState.sawSignal = true;
                }
            }

            long now = System.currentTimeMillis();
            if (now - lastLog > 2000) {
                lastLog = now;
                if (AppState.metersVisible && CaptureState.logEnabled) CaptureState.log(String.format(Locale.US,
                        "[电平] RMS=%.1f dBFS  峰值=%d  %.1f 秒",
                        CaptureState.rmsDb, CaptureState.peakAbs,
                        CaptureState.bytesTotal / (double) (SAMPLE_RATE * CHANNELS * BITS / 8)));
                if (server != null && AppState.metersVisible) {
                    // 显示本机 IP：接收端需要它来做直连排查（扫描不到时可以手动指定）
                    CaptureState.netInfo = String.format(Locale.US,
                            "广播中 · %s\n本机地址 %s  端口 %d\n已发送 %.1f MB   丢帧 %d   已批准 %d 个接收端",
                            Settings.deviceName(this),
                            AudioServer.localAddresses(),
                            Protocol.DISCOVERY_PORT,
                            server.bytesSent() / 1048576.0, server.framesDropped(),
                            server.approvedCount());
                }
            }
        }

        long secs = CaptureState.bytesTotal / (SAMPLE_RATE * CHANNELS * BITS / 8);
        CaptureState.log("[结论] 时长 " + secs + " 秒，总计 " + CaptureState.bytesTotal
                + " 字节，其中判定有信号 " + CaptureState.bytesSignal + " 字节，峰值 "
                + CaptureState.peakAbs);
        CaptureState.log(CaptureState.sawSignal
                ? "[结论] 成功：采到了声音，该应用允许被录制"
                : "[结论] 失败：全是静音，系统或目标应用禁止被录制");
        CaptureState.log("[采集] 读取线程结束");
        CaptureState.running = false;
    }

    // ------------------------------------------------------------------
    // 后台省电
    // ------------------------------------------------------------------
    /** 暂停采集（不销毁 AudioRecord，恢复时无需重新授权） */
    private void pauseCapture() {
        try {
            if (audioRecord != null
                    && audioRecord.getRecordingState() == AudioRecord.RECORDSTATE_RECORDING) {
                audioRecord.stop();
                CaptureState.log("[省电] 后台且无接收端连接，已暂停采集");
            }
        } catch (Throwable t) {
            CaptureState.log("[省电] 暂停采集失败: " + t);
        }
    }

    /** 恢复采集 */
    private void resumeCapture() {
        try {
            if (audioRecord != null
                    && audioRecord.getRecordingState() != AudioRecord.RECORDSTATE_RECORDING) {
                audioRecord.startRecording();
                CaptureState.log("[省电] 已恢复采集");
            }
        } catch (Throwable t) {
            CaptureState.log("[省电] 恢复采集失败: " + t);
        }
    }

    // ------------------------------------------------------------------
    // 带宽格式转换
    // ------------------------------------------------------------------
    /**
     * 把采集到的 48kHz 立体声字节流转成 short[]。
     *
     * 这里【不做】任何格式转换：降采样与单声道混合交给服务端，
     * 发送端统一通过 WiFi 向所有接收端发送 PCM。
     */
    private void toPcm48(byte[] src, int len) {
        int samples = len / 2;
        if (pcm48.length < samples) pcm48 = new short[samples];
        for (int i = 0; i < samples; i++) {
            int lo = src[i * 2] & 0xFF;
            int hi = src[i * 2 + 1];
            pcm48[i] = (short) ((hi << 8) | lo);
        }
    }

    /**
     * 运行中切换带宽档位。
     *
     * 只影响 WiFi 接收端。
     * 【不重启监听】：早期实现走 stop()+start() 重启，撞上 TIME_WAIT 导致端口
     * EADDRINUSE 绑不上，广播静默死亡。现在只给已连接的客户端补发一个 HELLO，
     * 接收端收到后会自行重建抖动缓冲。
     */
    private void applyBandwidth(int level) {
        try {
            bwLevel = Protocol.clampLevel(level);
            if (server != null && server.isRunning()) {
                server.setBandwidth(bwLevel);
            }
        } catch (Throwable t) {
            CaptureState.log("[带宽] 切换失败: " + t);
        }
    }

    // ------------------------------------------------------------------
    // 收尾
    // ------------------------------------------------------------------
    private void shutdown() {
        if (stopping) return;
        stopping = true;
        CaptureState.running = false;
        CaptureState.connectedSinceMs = 0;
        try {
            if (audioRecord != null) {
                try {
                    audioRecord.stop();
                } catch (Throwable ignored) {
                }
                audioRecord.release();
                audioRecord = null;
            }
        } catch (Throwable t) {
            CaptureState.log("[采集] 释放 AudioRecord 异常: " + t);
        }
        try {
            if (readerThread != null) readerThread.join(1500);
        } catch (InterruptedException ignored) {
        }
        if (server != null) {
            server.stop();
            server = null;
        }
        activeServer = null;
        if (announce != null) {
            announce.stop();
            announce = null;
        }
        activeAnnounce = null;
        try {
            if (projection != null) {
                if (projectionCallback != null) {
                    projection.unregisterCallback(projectionCallback);
                }
                projection.stop();
                projection = null;
            }
        } catch (Throwable t) {
            CaptureState.log("[投影] 释放异常: " + t);
        }
        stopForegroundCompat();
        stopSelf();
    }

    private void fail(String msg) {
        CaptureState.error = msg;
        CaptureState.log("[错误] " + msg);
        CaptureState.running = false;
        CaptureState.connectedSinceMs = 0;
        if (server != null) {
            server.stop();
            server = null;
        }
        activeServer = null;
        if (announce != null) {
            announce.stop();
            announce = null;
        }
        activeAnnounce = null;
        stopForegroundCompat();
        stopSelf();
    }

    // ------------------------------------------------------------------
    // 前台通知
    // ------------------------------------------------------------------
    private void startForegroundCompat() { startForegroundCompat(true); }

    @SuppressWarnings("deprecation")
    private void startForegroundCompat(boolean mediaProjection) {
        NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        if (nm != null && Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            NotificationChannel ch = new NotificationChannel(
                    CHANNEL_ID,
                    getString(R.string.notif_channel_name),
                    NotificationManager.IMPORTANCE_LOW);
            nm.createNotificationChannel(ch);
        }

        Intent open = new Intent(this, MainActivity.class);
        int piFlags = PendingIntent.FLAG_UPDATE_CURRENT;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            piFlags |= PendingIntent.FLAG_IMMUTABLE;
        }
        PendingIntent pi = PendingIntent.getActivity(this, 0, open, piFlags);

        Notification n = new Notification.Builder(this, CHANNEL_ID)
                .setContentTitle(getString(R.string.notif_title))
                .setContentText(getString(R.string.notif_text))
                .setSmallIcon(R.drawable.ic_audiohub_notification)
                .setContentIntent(pi)
                .setCategory(Notification.CATEGORY_SERVICE)
                .setAutoCancel(false)
                .setOnlyAlertOnce(true)
                .setOngoing(true)
                .build();

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            n.flags |= Notification.FLAG_ONGOING_EVENT;
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            int type = mediaProjection ? ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION
                    : ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC;
            startForeground(NOTIF_ID, n, type);
        } else {
            startForeground(NOTIF_ID, n);
        }
    }

    private void stopForegroundCompat() {
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
                stopForeground(STOP_FOREGROUND_REMOVE);
            } else {
                stopForeground(true);
            }
        } catch (Throwable ignored) {
        }
    }
}
