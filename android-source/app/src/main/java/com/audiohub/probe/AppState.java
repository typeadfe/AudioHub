package com.audiohub.probe;

/**
 * 应用级的运行期状态。
 *
 * `foreground` 由 Activity 生命周期维护（用计数而非布尔，因为可能同时有
 * MainActivity 与 SettingsActivity），用于后台省电判断：
 *   - 发送端：后台且无人连接时暂停采集（采集是最耗电的部分）
 *   - 接收端：后台照常播放（这是它的用途），但无活跃音源时暂停输出
 */
final class AppState {

    private AppState() {}

    private static int startedCount = 0;

    /** 是否有界面处于前台 */
    static volatile boolean foreground = false;

    static synchronized void onActivityStart() {
        startedCount++;
        foreground = startedCount > 0;
    }

    static synchronized void onActivityStop() {
        startedCount--;
        if (startedCount < 0) startedCount = 0;
        foreground = startedCount > 0;
    }
}
