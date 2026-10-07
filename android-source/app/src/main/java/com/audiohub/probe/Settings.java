package com.audiohub.probe;

import android.content.Context;
import android.content.SharedPreferences;
import android.os.Build;

/**
 * 应用设置（SharedPreferences 持久化）
 *
 * 关于「音频质量」的说明：
 *   本应用传输的是【无损 PCM】，没有有损压缩，因此不存在"码率高低"这种音质旋钮。
 *   真正影响听感的取舍是【抖动缓冲大小】：缓冲越小延迟越低，但 WiFi 抖动容易断音；
 *   缓冲越大越稳，但延迟更高。所以这里把它做成"质量与延迟"三档，名称如实反映其作用。
 */
final class Settings {

    private static final String PREF = "audiohub_settings";

    private static final String K_NAME      = "device_name";
    private static final String K_QUALITY   = "audio_quality";
    private static final String K_EXCLUSIVE = "exclusive_audio";
    private static final String K_AUTOCONN  = "auto_connect";
    private static final String K_BANDWIDTH = "bandwidth_level";
    private static final String K_LOG       = "log_enabled";
    private static final String K_APPROVAL  = "approval_required";
    private static final String K_APPROVED  = "approved_devices";
    private static final String K_DARK_MODE = "dark_mode";
    private static final String K_LANGUAGE = "language";

    /** 品质档位 */
    static final int Q_LOW_LATENCY = 0;   // 低延迟：20ms 缓冲
    static final int Q_BALANCED    = 1;   // 均衡：40ms 缓冲
    static final int Q_STABLE      = 2;   // 稳定优先：80ms 缓冲

    /** 设置页打开的项目仓库网页地址。 */
    static final String SOURCE_URL = "https://github.com/typeadfe/AudioHub";

    private Settings() {}

    private static SharedPreferences p(Context c) {
        return c.getApplicationContext().getSharedPreferences(PREF, Context.MODE_PRIVATE);
    }

    static String language(Context c) {
        String value = p(c).getString(K_LANGUAGE, "system");
        return "en".equals(value) || "zh".equals(value) ? value : "system";
    }

    static void setLanguage(Context c, String value) {
        p(c).edit().putString(K_LANGUAGE, value).apply();
    }

    static boolean darkMode(Context c) {
        return p(c).getBoolean(K_DARK_MODE, true);
    }

    static void setDarkMode(Context c, boolean enabled) {
        p(c).edit().putBoolean(K_DARK_MODE, enabled).apply();
    }

    // ---------------- 设备显示名称 ----------------
    static String deviceName(Context c) {
        String s = p(c).getString(K_NAME, null);
        if (s == null || s.trim().isEmpty()) return defaultDeviceName();
        return s.trim();
    }

    static void setDeviceName(Context c, String v) {
        String s = (v == null) ? "" : v.trim();
        if (s.length() > 40) s = s.substring(0, 40);
        p(c).edit().putString(K_NAME, s).apply();
    }

    /** 默认名称用厂商+型号，比"Android"更容易分辨 */
    static String defaultDeviceName() {
        String brand = Build.BRAND == null ? "" : Build.BRAND;
        String model = Build.MODEL == null ? "Android" : Build.MODEL;
        if (model.toLowerCase().startsWith(brand.toLowerCase())) return model;
        if (brand.isEmpty()) return model;
        return brand.substring(0, 1).toUpperCase() + brand.substring(1) + " " + model;
    }

    // ---------------- 音频质量与延迟 ----------------
    static int quality(Context c) {
        int q = p(c).getInt(K_QUALITY, Q_BALANCED);
        return (q < 0 || q > 2) ? Q_BALANCED : q;
    }

    static void setQuality(Context c, int q) {
        p(c).edit().putInt(K_QUALITY, q).apply();
    }

    /** 该档位对应的抖动缓冲目标（毫秒） */
    static int jitterTargetMs(Context c) {
        switch (quality(c)) {
            case Q_LOW_LATENCY: return 20;
            case Q_STABLE:      return 80;
            default:            return 40;
        }
    }

    static String qualityLabel(Context c) {
        switch (quality(c)) {
            case Q_LOW_LATENCY: return "低延迟";
            case Q_STABLE:      return "稳定优先";
            default:            return "均衡";
        }
    }

    // ---------------- 独占音频 ----------------
    /**
     * true  = 请求独占音频焦点，其他应用的声音会被暂停
     * false = 不请求焦点，与本机其他声音同时播放
     */
    static boolean exclusiveAudio(Context c) {
        return p(c).getBoolean(K_EXCLUSIVE, false);
    }

    static void setExclusiveAudio(Context c, boolean v) {
        p(c).edit().putBoolean(K_EXCLUSIVE, v).apply();
    }

    // ---------------- 自动连接新设备 ----------------
    static boolean autoConnect(Context c) {
        return p(c).getBoolean(K_AUTOCONN, true);
    }

    static void setAutoConnect(Context c, boolean v) {
        p(c).edit().putBoolean(K_AUTOCONN, v).apply();
    }

    // ---------------- 带宽档位 ----------------
    /**
     * 发送端使用的音频带宽档位（见 Protocol.BW_*）。
     * 通过降低采样率与声道数实现，不引入编解码器。
     */
    static int bandwidth(Context c) {
        return Protocol.clampLevel(p(c).getInt(K_BANDWIDTH, Protocol.BW_FULL));
    }

    static void setBandwidth(Context c, int level) {
        p(c).edit().putInt(K_BANDWIDTH, Protocol.clampLevel(level)).apply();
    }

    static String bandwidthLabel(Context c) {
        int l = bandwidth(c);
        return Protocol.bwLabel(l) + " · " + Protocol.bwFormatText(l)
                + " · " + Protocol.bwKbps(l) + " kbps";
    }

    // ---------------- 运行日志 ----------------
    /** 默认关闭：长期运行时记录日志既耗电又占内存，只在排查问题时才需要 */
    static boolean logEnabled(Context c) {
        return p(c).getBoolean(K_LOG, false);
    }

    static void setLogEnabled(Context c, boolean v) {
        p(c).edit().putBoolean(K_LOG, v).apply();
    }

    /** 把设置同步到运行期状态（服务与界面都会调用） */
    static void applyRuntime(Context c) {
        CaptureState.logEnabled = logEnabled(c);
        PlayerJitterBuffer.setTargetMs(jitterTargetMs(c));
    }

    // ---------------- 连接审批（安全） ----------------
    /**
     * 是否需要人工批准新接入的接收端。默认【开启】。
     *
     * 开启后，接收端连上来不会立刻收到音频，必须在发送端界面上点【允许】。
     * 这样即使有人在局域网里写个脚本连上端口，也拿不到任何声音。
     */
    static boolean approvalRequired(Context c) {
        return p(c).getBoolean(K_APPROVAL, true);
    }

    static void setApprovalRequired(Context c, boolean v) {
        p(c).edit().putBoolean(K_APPROVAL, v).apply();
    }

    /** 已批准设备的标识：IP + 设备名（名字仅作显示，匹配时只看 IP） */
    static String deviceKey(String addr, String name) {
        String a = (addr == null) ? "" : addr.trim();
        String n = (name == null) ? "" : name.trim();
        return a + "|" + n;
    }

    /** 从标识里取出 IP */
    static String addrOf(String key) {
        int bar = (key == null) ? -1 : key.indexOf('|');
        return (bar >= 0) ? key.substring(0, bar) : (key == null ? "" : key);
    }

    /** 从标识里取出设备名 */
    static String nameOf(String key) {
        int bar = (key == null) ? -1 : key.indexOf('|');
        return (bar >= 0 && bar + 1 <= key.length()) ? key.substring(bar + 1) : "";
    }

    static java.util.Set<String> approvedDevices(Context c) {
        java.util.Set<String> out = new java.util.HashSet<>();
        String raw = p(c).getString(K_APPROVED, "");
        if (raw == null || raw.isEmpty()) return out;
        for (String s : raw.split("\n")) {
            if (!s.trim().isEmpty()) out.add(s);
        }
        return out;
    }

    /**
     * 是否已批准。
     *
     * 【按 IP 匹配，不看设备名】。原因：设备名会变（用户改名、我改过程序里的
     * 默认名），若把名字也算进匹配条件，改个名字就会要求重新批准，
     * 而且列表里会留下多条同 IP 的过期记录 —— 用户看到的就是"列表显示不正确"。
     */
    static boolean isApproved(Context c, String addr, String name) {
        String a = (addr == null) ? "" : addr.trim();
        for (String k : approvedDevices(c)) {
            if (addrOf(k).equals(a)) return true;
        }
        return false;
    }

    /** 批准一台设备；同一 IP 只保留一条记录，并更新为最新的名字 */
    static void approve(Context c, String addr, String name) {
        java.util.Set<String> s = approvedDevices(c);
        String a = (addr == null) ? "" : addr.trim();
        java.util.Iterator<String> it = s.iterator();
        while (it.hasNext()) {
            if (addrOf(it.next()).equals(a)) it.remove();
        }
        s.add(deviceKey(a, name));
        saveApproved(c, s);
    }

    static void revoke(Context c, String addr, String name) {
        java.util.Set<String> s = approvedDevices(c);
        String a = (addr == null) ? "" : addr.trim();
        java.util.Iterator<String> it = s.iterator();
        while (it.hasNext()) {
            if (addrOf(it.next()).equals(a)) it.remove();
        }
        saveApproved(c, s);
    }

    static void clearApproved(Context c) {
        p(c).edit().remove(K_APPROVED).apply();
    }

    private static void saveApproved(Context c, java.util.Set<String> s) {
        p(c).edit().putString(K_APPROVED, android.text.TextUtils.join("\n", s)).apply();
    }
}
