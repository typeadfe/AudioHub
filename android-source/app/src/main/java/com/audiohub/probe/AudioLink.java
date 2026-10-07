package com.audiohub.probe;

import java.io.InputStream;
import java.io.OutputStream;
import java.net.Socket;

/** TCP 音频链路。AudioHub 使用 WiFi 传输音频，接收端主动连接发送端。 */
interface AudioLink {
    InputStream in() throws Exception;
    OutputStream out() throws Exception;
    String remoteAddr();
    boolean isAlive();
    void close();
    String kind();

    final class Tcp implements AudioLink {
        private final Socket s;
        private final String addr;
        private volatile boolean alive = true;
        Tcp(Socket s) {
            this.s = s;
            String a;
            try { a = s.getInetAddress().getHostAddress(); } catch (Exception e) { a = "?"; }
            this.addr = a;
        }
        Socket socket() { return s; }
        @Override public InputStream in() throws Exception { return s.getInputStream(); }
        @Override public OutputStream out() throws Exception { return s.getOutputStream(); }
        @Override public String remoteAddr() { return addr; }
        @Override public boolean isAlive() { return alive && !s.isClosed(); }
        @Override public void close() { alive = false; try { s.close(); } catch (Exception ignored) {} }
        @Override public String kind() { return "WiFi"; }
    }
}
