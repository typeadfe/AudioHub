package com.audiohub.probe;

import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import android.widget.Toast;

/** 网络线程收到剪贴板消息后，在主线程显示短提示。 */
final class ClipboardNotice {
    private static final Handler MAIN = new Handler(Looper.getMainLooper());

    private ClipboardNotice() {}

    static void show(Context context, String message) {
        Context app = context.getApplicationContext();
        MAIN.post(() -> Toast.makeText(app, message, Toast.LENGTH_SHORT).show());
    }
}
