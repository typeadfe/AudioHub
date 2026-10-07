package com.audiohub.probe;

/**
 * 采集状态共享区。
 * CaptureService 负责写入；主界面可见时才轮询并刷新界面。
 * 用 volatile + synchronized 保证跨线程可见性，避免引入额外依赖。
 */
final class CaptureState {

    /** logcat 标签 */
    static final String TAG = "AudioHubProbe";

    private CaptureState() {}

    /** 是否正在采集 */
    static volatile boolean running = false;

    /** 是否读到过有效（非静音）信号 —— 这是判定的核心依据 */
    static volatile boolean sawSignal = false;

    /** 当前电平：0~100，用于进度条 */
    static volatile int levelPercent = 0;

    /** 当前 RMS 分贝值 */
    static volatile float rmsDb = -120f;

    /** At least one approved receiver has been connected since this elapsed time. */
    static volatile long connectedSinceMs = 0;

    /** 本次采集过程中的最大绝对值（0~32767） */
    static volatile int peakAbs = 0;

    /** 累计读到的总字节数 */
    static volatile long bytesTotal = 0;

    /** 累计"有明显信号"的字节数 */
    static volatile long bytesSignal = 0;

    /** AudioRecord.getState() 结果，STATE_INITIALIZED=1 */
    static volatile int recordState = -1;

    /** 出错信息 */
    static volatile String error = null;

    /** 网络推流状态文本（由服务定期刷新） */
    static volatile String netInfo = "未启动";

    /**
     * 本机输出增益（0.0 ~ 2.0），由界面滑块控制。
     * 在【采集之后、发送之前】生效，因此播放端收到什么就是什么 ——
     * 每台手机自己决定自己这一路有多响，不必跑去电脑上调。
     */
    static volatile float inputGain = 1.0f;

    private static final StringBuilder LOG = new StringBuilder();

    /**
     * 是否记录运行日志。由设置里的开关控制。
     *
     * 默认【关闭】：每条日志都要拼字符串并写 logcat，长期运行下既耗电又占内存，
     * 而这些信息只在排查问题时才需要。开启后实时生效，无需重启。
     */
    static volatile boolean logEnabled = false;

    /** 日志上限，防止长时间运行无限增长 */
    private static final int LOG_MAX_CHARS = 24000;
    private static final int LOG_TRIM_CHARS = 8000;

    static synchronized void log(String line) {
        if (!logEnabled) return;
        LOG.append(line).append('\n');
        if (LOG.length() > LOG_MAX_CHARS) LOG.delete(0, LOG_TRIM_CHARS);
        android.util.Log.i(TAG, line);
    }

    static synchronized String logText() {
        return LOG.toString();
    }

    static synchronized void clearLog() {
        LOG.setLength(0);
    }

    /** 开始新一轮采集前重置统计 */
    static void reset() {
        sawSignal = false;
        levelPercent = 0;
        rmsDb = -120f;
        connectedSinceMs = 0;
        peakAbs = 0;
        bytesTotal = 0;
        bytesSignal = 0;
        recordState = -1;
        error = null;
        netInfo = "未启动";
        clearLog();    }
}
