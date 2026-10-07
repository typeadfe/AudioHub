package com.audiohub.probe;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;

/**
 * 播放端前台服务。
 *
 * 为什么需要：Android 在应用退到后台后会限制普通进程，音频会断。
 * 用前台服务 + 常驻通知，才能像音乐播放器一样在后台持续出声。
 */
public class PlayerService extends Service {

    static final String ACTION_START = "com.audiohub.probe.PLAYER_START";
    static final String ACTION_STOP  = "com.audiohub.probe.PLAYER_STOP";

    private static final int    NOTIF_ID   = 0x4156;
    private static final String CHANNEL_ID = "audiohub_player";

    private static PlayerEngine engine;

    /** 全局单例：界面与服务共用同一个引擎 */
    public static synchronized PlayerEngine engine(android.content.Context c) {
        if (engine == null) engine = new PlayerEngine(c);
        return engine;
    }

    public static synchronized PlayerEngine engineOrNull() { return engine; }

    @Override
    public IBinder onBind(Intent intent) { return null; }

    @Override
    public void onCreate() {
        super.onCreate();
        Settings.applyRuntime(this);   // 日志开关、缓冲档位
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        String action = (intent == null) ? null : intent.getAction();

        if (ACTION_STOP.equals(action)) {
            PlayerEngine e = engine(this);
            if (e.isRunning()) e.stop();
            stopForegroundCompat();
            stopSelf();
            return START_NOT_STICKY;
        }

        startForegroundCompat();

        PlayerEngine e = engine(this);
        e.setListener(null);   // 监听器由界面自行注册，避免持有已销毁的 Activity
        if (!e.isRunning()) {
            if (!e.start()) {
                CaptureState.log("[接收] 引擎启动失败（AudioTrack 不可用）");
            }
        }
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        super.onDestroy();
        // 不在这里 stop 引擎：界面可能只是临时切走
    }

    // ------------------------------------------------------------------
    private void startForegroundCompat() {
        NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        if (nm != null && Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            NotificationChannel ch = new NotificationChannel(
                    CHANNEL_ID,
                    getString(R.string.notif_channel_player),
                    NotificationManager.IMPORTANCE_LOW);
            nm.createNotificationChannel(ch);
        }

        Intent open = new Intent(this, MainActivity.class);
        int piFlags = PendingIntent.FLAG_UPDATE_CURRENT;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) piFlags |= PendingIntent.FLAG_IMMUTABLE;
        PendingIntent pi = PendingIntent.getActivity(this, 1, open, piFlags);

        Notification n = new Notification.Builder(this, CHANNEL_ID)
                .setContentTitle(getString(R.string.notif_player_title))
                .setContentText(getString(R.string.notif_player_text))
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
            startForeground(NOTIF_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK);
        } else {
            startForeground(NOTIF_ID, n);
        }
    }

    private void stopForegroundCompat() {
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) stopForeground(STOP_FOREGROUND_REMOVE);
            else stopForeground(true);
        } catch (Throwable ignored) {}
    }
}
