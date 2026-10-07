package com.audiohub.probe;

import android.content.Context;
import android.content.res.Configuration;
import java.util.Locale;

/** Small runtime labels used by dynamically assembled status rows. */
final class UiText {
    private UiText() {}

    static boolean english(Context context) {
        String selected = Settings.language(context);
        if ("en".equals(selected)) return true;
        if ("zh".equals(selected)) return false;
        Locale locale = context.getResources().getConfiguration().getLocales().get(0);
        return "en".equals(locale.getLanguage());
    }

    static String resource(Context context, int id) {
        String selected = Settings.language(context);
        if ("system".equals(selected)) return context.getString(id);
        Configuration config = new Configuration(context.getResources().getConfiguration());
        config.setLocale(Locale.forLanguageTag(selected));
        return context.createConfigurationContext(config).getString(id);
    }

    static String tr(Context context, String chinese, String english) {
        return english(context) ? english : chinese;
    }

    static String status(Context context, String value) {
        if (!english(context) || value == null) return value;
        switch (value) {
            case "等待连接": return "Waiting to connect";
            case "已连接": return "Connected";
            case "等待发送端确认": return "Waiting for sender approval";
            case "已被发送端拒绝": return "Rejected by sender";
            case "连接失败，重试中": return "Connection failed; retrying";
            case "已断开，重连中": return "Disconnected; reconnecting";
            case "已停止": return "Stopped";
            case "未启动": return "Not started";
            case "拿不到 MediaProjectionManager": return "Screen capture service unavailable";
            case "MediaProjection 为 null（授权被拒绝或令牌无效）":
                return "Capture permission denied or expired";
            default: return value;
        }
    }

    static String format(Context context, String value) {
        if (!english(context) || value == null) return value;
        return value.replace("单声道", "mono").replace("立体声", "stereo")
                .replace("高保真", "High fidelity").replace("标准", "Standard")
                .replace("省流", "Data saving").replace("无损原音", "Original quality");
    }

    static String bandwidth(Context context, int level) {
        if (!english(context)) return Protocol.bwLabel(level);
        switch (Protocol.clampLevel(level)) {
            case Protocol.BW_HIGH: return "High fidelity";
            case Protocol.BW_MED: return "Standard";
            case Protocol.BW_LOW: return "Data saving";
            default: return "Original quality";
        }
    }
}
