package com.audiohub.probe;

import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.InterfaceAddress;
import java.net.NetworkInterface;
import java.net.Socket;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.BooleanSupplier;

/**
 * 局域网扫描：找出正在监听指定端口的采集端手机。
 *
 * 与 Windows 端同样的思路 —— 播放端【主动出站连接】，
 * 既不受任何防火墙入站拦截影响，也不依赖 UDP 广播（常被 AP 隔离挡住）。
 */
final class LanScan {

    private LanScan() {}

    /** 本机所有可用的 IPv4 地址 */
    static List<String> localIPv4() {
        List<String> out = new ArrayList<>();
        try {
            for (NetworkInterface ni : Collections.list(NetworkInterface.getNetworkInterfaces())) {
                if (!ni.isUp() || ni.isLoopback()) continue;
                String iface = ni.getName() == null ? "" : ni.getName().toLowerCase(java.util.Locale.ROOT);
                // 手动扫描也只查实际局域网，不把 VPN/TUN 或移动数据网段交给代理处理。
                if (ni.isVirtual() || iface.startsWith("tun") || iface.startsWith("tap")
                        || iface.startsWith("ppp") || iface.startsWith("rmnet")
                        || iface.startsWith("ccmni") || iface.startsWith("pdp")
                        || iface.startsWith("v4-")) continue;
                for (InterfaceAddress ia : ni.getInterfaceAddresses()) {
                    InetAddress a = ia.getAddress();
                    if (a instanceof Inet4Address && !a.isLoopbackAddress()) {
                        String ip = a.getHostAddress();
                        if (ip == null || ip.startsWith("169.254.")) continue;
                        if (!out.contains(ip)) out.add(ip);
                    }
                }
            }
        } catch (Exception ignored) {}
        Collections.sort(out);
        return out;
    }

    /** 本机各网段的候选地址（.1 ~ .254） */
    static Set<String> candidates() {
        Set<String> out = new HashSet<>();
        for (String ip : localIPv4()) {
            int p = ip.lastIndexOf('.');
            if (p <= 0) continue;
            String prefix = ip.substring(0, p);
            for (int i = 1; i <= 254; i++) out.add(prefix + "." + i);
        }
        return out;
    }

    /** 并发扫描。返回接受连接的主机（已排序） */
    static List<String> scan(int port, int timeoutMs, BooleanSupplier keepRunning) {
        List<String> hosts = new ArrayList<>(candidates());
        if (hosts.isEmpty()) return new ArrayList<>();

        List<String> found = Collections.synchronizedList(new ArrayList<String>());
        AtomicInteger idx = new AtomicInteger(0);
        int n = Math.min(16, hosts.size());
        long deadline = android.os.SystemClock.elapsedRealtime() + 5000;
        Thread[] ts = new Thread[n];
        for (int i = 0; i < n; i++) {
            ts[i] = new Thread(() -> {
                while (true) {
                    if (!keepRunning.getAsBoolean()
                            || android.os.SystemClock.elapsedRealtime() >= deadline) return;
                    int k = idx.getAndIncrement();
                    if (k >= hosts.size()) return;
                    String ip = hosts.get(k);
                    if (tryConnect(ip, port, timeoutMs)) found.add(ip);
                }
            }, "ahub-scan");
            ts[i].start();
        }
        for (Thread t : ts) {
            try { t.join(); } catch (InterruptedException ignored) {}
        }
        Collections.sort(found);
        return found;
    }

    private static boolean tryConnect(String ip, int port, int timeoutMs) {
        Socket s = new Socket();
        try {
            s.connect(new InetSocketAddress(ip, port), timeoutMs);
            s.setSoTimeout(Math.max(timeoutMs, 350));
            InputStream in = s.getInputStream();
            byte[] hdr = new byte[Protocol.HDR_COMMON];
            int off = 0;
            while (off < hdr.length) {
                int n = in.read(hdr, off, hdr.length - off);
                if (n <= 0) return false;
                off += n;
            }
            return Protocol.validHeader(hdr, hdr.length)
                    && Protocol.typeOf(hdr) == Protocol.TYPE_HELLO
                    && Protocol.bodyLenOf(hdr) >= 3
                    && Protocol.bodyLenOf(hdr) <= 64;
        } catch (Exception e) {
            return false;
        } finally {
            try { s.close(); } catch (Exception ignored) {}
        }
    }
}
