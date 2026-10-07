package com.audiohub.probe;

import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;

/**
 * 周期性广播"本机是一个可用接收端"。
 *
 * 【为什么必须有它】
 * 连接方向是"接收端主动连发送端"（为绕开 Windows 防火墙入站拦截），
 * 因此发送端【无法主动发现】接收端 —— 对方不连过来，发送端界面上就是空的。
 *
 * 早期只在 Windows 播放端实现了这个广播，Android 接收端没有做，
 * 结果两台手机之间"先开接收端、再开发送端"时，发送端永远找不到接收端。
 *
 * UDP 是【出站】发送，不受任何防火墙入站规则影响。
 *
 * 目标地址同时发 255.255.255.255 和本机网段的定向广播地址 ——
 * 有限广播在有 VPN / 虚拟网卡时可能走错网卡，加发定向广播才稳。
 */
final class AnnounceSender {

    /** 广播间隔（毫秒） */
    private static final int INTERVAL_MS = 5000;

    private final String deviceName;
    private volatile boolean running = false;
    private DatagramSocket sock;
    private Thread thread;

    AnnounceSender(String deviceName) {
        this.deviceName = (deviceName == null || deviceName.isEmpty())
                ? "Android" : deviceName;
    }

    void start() {
        if (running) return;
        try {
            sock = new DatagramSocket();
            sock.setBroadcast(true);
        } catch (Exception e) {
            CaptureState.log("[发现] 启动接收端广播失败: " + e);
            sock = null;
            return;
        }
        running = true;
        thread = new Thread(this::loop, "ahub-announce-tx");
        thread.start();
        CaptureState.log("[发现] 已开始广播本机是可用接收端（UDP "
                + Protocol.ANNOUNCE_PORT + "）");
    }

    void stop() {
        running = false;
        try { if (sock != null) sock.close(); } catch (Exception ignored) {}
        if (thread != null) thread.interrupt();
        try { if (thread != null) thread.join(600); } catch (InterruptedException ignored) {}
        sock = null;
        thread = null;
    }

    boolean isRunning() { return running; }

    private void loop() {
        byte[] payload = Protocol.buildAnnounce(deviceName);
        while (running) {
            try {
                // 定向广播：按 /24 推算网段广播地址
                for (String ip : LanScan.localIPv4()) {
                    int dot = ip.lastIndexOf('.');
                    if (dot <= 0) continue;
                    sendTo(payload, ip.substring(0, dot) + ".255");
                }
                sendTo(payload, "255.255.255.255");
            } catch (Throwable ignored) {}
            try { Thread.sleep(INTERVAL_MS); } catch (InterruptedException e) { return; }
        }
    }

    private void sendTo(byte[] payload, String addr) {
        try {
            InetAddress a = InetAddress.getByName(addr);
            DatagramPacket p = new DatagramPacket(payload, payload.length,
                    a, Protocol.ANNOUNCE_PORT);
            sock.send(p);
        } catch (Throwable ignored) {}
    }
}
