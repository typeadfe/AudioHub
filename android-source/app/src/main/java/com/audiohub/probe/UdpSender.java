package com.audiohub.probe;

import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.InterfaceAddress;
import java.net.NetworkInterface;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Iterator;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicLong;

/**
 * UDP 音频发送端（对应 AudioRelay 里的 Server 角色）
 *
 * 流程：
 *   1. 每秒向局域网广播一次 DISCOVER（广播包很小，不占带宽）
 *   2. 播放端收到后单播回 REPLY，告知它的数据端口
 *   3. 之后把 5ms/960 字节的音频帧单播发给每一个已知播放端
 *
 * 优点：一个源可以同时发给多个播放端（顺带保留了 AudioRelay 的原有能力），
 *       而且不需要在手机上手动填 IP。
 */
public final class UdpSender {

    private static final String TAG = "AudioHubProbe";

    /** 播放端多久没回应就认为掉线（毫秒） */
    private static final long PLAYER_TIMEOUT_MS = 5000;

    private final int sourceId;
    private final String deviceName;

    private DatagramSocket socket;
    private Thread rxThread;
    private Thread discoverThread;
    private volatile boolean running = false;

    private final Map<String, Player> players = new ConcurrentHashMap<>();
    private volatile InetSocketAddress manualTarget = null;

    private final AtomicLong audioPackets = new AtomicLong();
    private final AtomicLong audioBytes   = new AtomicLong();
    private final AtomicLong discoverSent = new AtomicLong();
    private final AtomicLong repliesGot   = new AtomicLong();
    private final AtomicLong sendErrors   = new AtomicLong();

    public static final class Player {
        public final String key;
        public final InetSocketAddress addr;
        public volatile long lastSeenMs;
        Player(String key, InetSocketAddress addr, long now) {
            this.key = key; this.addr = addr; this.lastSeenMs = now;
        }
        @Override public String toString() { return addr.getAddress().getHostAddress() + ":" + addr.getPort(); }
    }

    public UdpSender(int sourceId, String deviceName) {
        this.sourceId   = sourceId;
        this.deviceName = deviceName == null ? "Android" : deviceName;
    }

    public int sourceId() { return sourceId; }
    public boolean isRunning() { return running; }
    public long audioPackets() { return audioPackets.get(); }
    public long audioBytes()   { return audioBytes.get(); }
    public long discoverSent() { return discoverSent.get(); }
    public long repliesGot()   { return repliesGot.get(); }
    public long sendErrors()   { return sendErrors.get(); }

    /** 返回当前在线的播放端描述 */
    public List<String> playerList() {
        long now = System.currentTimeMillis();
        List<String> out = new ArrayList<>();
        for (Player p : players.values()) {
            if (now - p.lastSeenMs < PLAYER_TIMEOUT_MS) out.add(p.toString());
        }
        Collections.sort(out);
        return out;
    }

    /** 手动指定播放端（自动发现失败时的兜底） */
    public void setManualTarget(String ip, int port) {
        if (ip == null || ip.trim().isEmpty()) { manualTarget = null; return; }
        try {
            manualTarget = new InetSocketAddress(InetAddress.getByName(ip.trim()), port);
            CaptureState.log("[网络] 手动指定播放端 " + ip.trim() + ":" + port);
        } catch (Exception e) {
            manualTarget = null;
            CaptureState.log("[网络] 手动播放端地址无效: " + e);
        }
    }

    // ------------------------------------------------------------------
    public boolean start() {
        if (running) return true;
        try {
            socket = new DatagramSocket();          // 系统分配临时端口
            socket.setBroadcast(true);
            socket.setSoTimeout(500);               // 让接收线程能周期性检查退出
        } catch (Exception e) {
            CaptureState.log("[网络] 创建 socket 失败: " + e);
            return false;
        }
        running = true;
        CaptureState.log("[网络] 发送端已启动，本地端口 " + socket.getLocalPort()
                + "，sourceId=0x" + Integer.toHexString(sourceId));

        rxThread = new Thread(this::rxLoop, "ahub-rx");
        rxThread.start();
        discoverThread = new Thread(this::discoverLoop, "ahub-discover");
        discoverThread.start();
        return true;
    }

    public void stop() {
        if (!running) return;
        running = false;
        sendBye();
        try { if (socket != null) socket.close(); } catch (Exception ignored) {}
        try { if (rxThread != null) rxThread.join(800); } catch (InterruptedException ignored) {}
        try { if (discoverThread != null) discoverThread.join(800); } catch (InterruptedException ignored) {}
        socket = null;
        players.clear();
        CaptureState.log("[网络] 发送端已停止");
    }

    // ------------------------------------------------------------------
    /** 发送一帧音频（pkt 已按协议填好） */
    public void sendAudioPacket(byte[] pkt, int len) {
        if (!running || socket == null) return;

        // 自动发现的播放端
        long now = System.currentTimeMillis();
        boolean any = false;
        for (Player p : players.values()) {
            if (now - p.lastSeenMs >= PLAYER_TIMEOUT_MS) continue;
            try {
                socket.send(new DatagramPacket(pkt, len, p.addr));
                any = true;
            } catch (Exception e) {
                sendErrors.incrementAndGet();
            }
        }
        // 手动指定的播放端
        if (manualTarget != null) {
            try {
                socket.send(new DatagramPacket(pkt, len, manualTarget));
                any = true;
            } catch (Exception e) {
                sendErrors.incrementAndGet();
            }
        }

        if (any) {
            audioPackets.incrementAndGet();
            audioBytes.addAndGet(len);
        }
    }

    // ------------------------------------------------------------------
    private List<InetAddress> broadcastAddresses() {
        List<InetAddress> out = new ArrayList<>();
        try {
            for (NetworkInterface ni : Collections.list(NetworkInterface.getNetworkInterfaces())) {
                if (!ni.isUp() || ni.isLoopback()) continue;
                for (InterfaceAddress ia : ni.getInterfaceAddresses()) {
                    InetAddress b = ia.getBroadcast();
                    if (b != null) out.add(b);
                }
            }
        } catch (Exception ignored) {}
        if (out.isEmpty()) {
            try { out.add(InetAddress.getByName("255.255.255.255")); } catch (Exception ignored) {}
        }
        return out;
    }

    private void discoverLoop() {
        byte[] pkt = new byte[Protocol.HDR_COMMON + 3 + 64];
        byte[] nameBytes = deviceName.getBytes();
        int nameLen = Math.min(nameBytes.length, 64);

        Protocol.writeHeader(pkt, Protocol.TYPE_DISCOVER, sourceId, 3 + nameLen);
        Protocol.putShortLE(pkt, Protocol.HDR_COMMON, socket.getLocalPort());
        pkt[Protocol.HDR_COMMON + 2] = (byte) nameLen;
        System.arraycopy(nameBytes, 0, pkt, Protocol.HDR_COMMON + 3, nameLen);
        int pktLen = Protocol.HDR_COMMON + 3 + nameLen;

        List<InetAddress> bcasts = broadcastAddresses();
        StringBuilder sb = new StringBuilder();
        for (InetAddress a : bcasts) sb.append(a.getHostAddress()).append(' ');
        CaptureState.log("[网络] 广播地址: " + sb.toString().trim());

        while (running) {
            for (InetAddress a : bcasts) {
                try {
                    socket.send(new DatagramPacket(pkt, pktLen,
                            new InetSocketAddress(a, Protocol.DISCOVERY_PORT)));
                    discoverSent.incrementAndGet();
                } catch (Exception ignored) {}
            }
            try { Thread.sleep(Protocol.DISCOVER_INTERVAL_MS); } catch (InterruptedException e) { break; }
        }
    }

    private void rxLoop() {
        byte[] buf = new byte[2048];
        while (running) {
            DatagramPacket dp = new DatagramPacket(buf, buf.length);
            try {
                socket.receive(dp);
            } catch (java.net.SocketTimeoutException e) {
                prunePlayers();
                continue;
            } catch (Exception e) {
                if (running) CaptureState.log("[网络] 接收异常: " + e);
                break;
            }

            int len = dp.getLength();
            if (!Protocol.validHeader(buf, len)) continue;
            byte type = Protocol.typeOf(buf);

            if (type == Protocol.TYPE_REPLY) {
                int dataPort = Protocol.dataPortOf(buf);
                if (dataPort <= 0) continue;
                InetAddress from = dp.getAddress();
                String key = from.getHostAddress() + ":" + dataPort;
                long now = System.currentTimeMillis();
                Player p = players.get(key);
                if (p == null) {
                    p = new Player(key, new InetSocketAddress(from, dataPort), now);
                    players.put(key, p);
                    CaptureState.log("[网络] 发现播放端 " + key + "，开始推流");
                } else {
                    p.lastSeenMs = now;
                }
                repliesGot.incrementAndGet();
            }
        }
    }

    private void prunePlayers() {
        long now = System.currentTimeMillis();
        Iterator<Map.Entry<String, Player>> it = players.entrySet().iterator();
        while (it.hasNext()) {
            Player p = it.next().getValue();
            if (now - p.lastSeenMs >= PLAYER_TIMEOUT_MS) {
                CaptureState.log("[网络] 播放端掉线 " + p.key);
                it.remove();
            }
        }
    }

    private void sendBye() {
        if (socket == null) return;
        try {
            byte[] pkt = new byte[Protocol.HDR_COMMON];
            Protocol.writeHeader(pkt, Protocol.TYPE_BYE, sourceId, 0);
            for (InetAddress a : broadcastAddresses()) {
                try {
                    socket.send(new DatagramPacket(pkt, pkt.length,
                            new InetSocketAddress(a, Protocol.DISCOVERY_PORT)));
                } catch (Exception ignored) {}
            }
        } catch (Exception ignored) {}
    }
}
