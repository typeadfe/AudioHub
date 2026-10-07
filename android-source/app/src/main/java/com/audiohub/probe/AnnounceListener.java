package com.audiohub.probe;

import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.util.ArrayList;
import java.util.Iterator;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

/**
 * 监听"接收端存在"广播。
 *
 * 【为什么需要】
 * 连接方向是"接收端主动连发送端"（为绕开 Windows 防火墙入站拦截），
 * 因此发送端无法主动发现接收端 —— 对方不连过来，发送端界面上就是空的。
 *
 * 让接收端（电脑播放端、以及将来可能的其它播放端）周期性 UDP 广播自己的存在，
 * 发送端监听后就能列出"可用但尚未连接"的接收端，界面与「接收」模式对称。
 *
 * UDP 是【出站】发送（接收端发出），所以不受任何防火墙入站规则影响；
 * 手机端只需监听，Android 不会拦入站 UDP。
 */
final class AnnounceListener {

    /** 超过这个时间没收到广播就认为对方已下线 */
    private static final long TTL_MS = 16000;

    private static final class Entry {
        final String name;
        volatile long lastMs;
        Entry(String name, long lastMs) { this.name = name; this.lastMs = lastMs; }
    }

    /** addr -> 设备信息 */
    private final Map<String, Entry> seen = new ConcurrentHashMap<>();
    private volatile boolean running = false;
    private DatagramSocket sock;
    private Thread thread;

    void start() {
        if (running) return;
        try {
            // ⚠️ 必须先创建【未绑定】的 DatagramSocket、设置 SO_REUSEADDR、再 bind。
            //    `new DatagramSocket(port)` 在构造时就已经 bind 了，之后再设
            //    setReuseAddress 完全无效。切模式时上一次的监听可能还没完全释放，
            //    这时 bind 会直接失败 → 监听器是死的 → 界面永远收不到接收端广播。
            //    （TCP 那边踩过同样的坑，见 AudioServer.start）
            DatagramSocket s = new DatagramSocket(null);
            s.setReuseAddress(true);
            s.setBroadcast(true);
            s.bind(new java.net.InetSocketAddress(Protocol.ANNOUNCE_PORT));
            sock = s;
        } catch (Exception e) {
            CaptureState.log("[发现] 监听接收端广播失败（端口 "
                    + Protocol.ANNOUNCE_PORT + "）: " + e);
            sock = null;
            return;
        }
        running = true;
        thread = new Thread(this::loop, "ahub-announce");
        thread.start();
        CaptureState.log("[发现] 已开始监听接收端广播（UDP " + Protocol.ANNOUNCE_PORT + "）");
    }

    void stop() {
        running = false;
        try { if (sock != null) sock.close(); } catch (Exception ignored) {}
        try { if (thread != null) thread.join(500); } catch (InterruptedException ignored) {}
        sock = null;
        thread = null;
        seen.clear();
    }

    boolean isRunning() { return running; }

    private void loop() {
        byte[] buf = new byte[512];
        while (running) {
            try {
                DatagramPacket p = new DatagramPacket(buf, buf.length);
                sock.receive(p);
                if (p.getLength() < Protocol.HDR_COMMON) continue;
                if (!Protocol.validHeader(buf, p.getLength())) continue;
                String addr = p.getAddress().getHostAddress();
                if (Protocol.typeOf(buf) == Protocol.TYPE_BYE) {
                    seen.remove(addr);
                    continue;
                }
                if (Protocol.typeOf(buf) != Protocol.TYPE_ANNOUNCE) continue;
                // 忽略自己发的（同一台设备既发送又接收时）
                if (LanScan.localIPv4().contains(addr)) continue;

                String name = Protocol.parseAnnounceName(buf, p.getLength());
                if (name.isEmpty()) name = addr;
                seen.put(addr, new Entry(name, System.currentTimeMillis()));
            } catch (Exception e) {
                if (running) {
                    try { Thread.sleep(200); } catch (InterruptedException ignored) { return; }
                }
            }
        }
    }

    /**
     * 当前可用的接收端：返回 [addr, name, 是否已连接(false)] 三元组。
     * 同时清理超时未广播的条目。
     */
    List<String[]> available() {
        long now = System.currentTimeMillis();
        List<String[]> out = new ArrayList<>();
        Iterator<Map.Entry<String, Entry>> it = seen.entrySet().iterator();
        while (it.hasNext()) {
            Map.Entry<String, Entry> e = it.next();
            Entry v = e.getValue();
            if (now - v.lastMs > TTL_MS) {
                it.remove();
                continue;
            }
            out.add(new String[]{ e.getKey(), v.name });
        }
        return out;
    }
}
