#include "control_server.h"

#include <cstdio>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")

namespace ahub {

ControlServer::~ControlServer() { stop(); }

bool ControlServer::start(uint16_t port, Handler handler, std::string& err) {
    handler_ = std::move(handler);
    port_    = port;

    listenSock_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock_ == INVALID_SOCKET) { err = "控制服务创建 socket 失败"; return false; }

    BOOL yes = TRUE;
    setsockopt(listenSock_, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

    sockaddr_in a{};
    a.sin_family      = AF_INET;
    a.sin_port        = htons(port);
    // 只绑回环：控制接口不对外暴露
    a.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (bind(listenSock_, (sockaddr*)&a, sizeof(a)) == SOCKET_ERROR) {
        char buf[128];
        sprintf(buf, "控制服务绑定 127.0.0.1:%u 失败（错误码 %d）", port, WSAGetLastError());
        err = buf;
        closesocket(listenSock_);
        listenSock_ = INVALID_SOCKET;
        return false;
    }
    if (listen(listenSock_, 8) == SOCKET_ERROR) {
        err = "控制服务 listen 失败";
        closesocket(listenSock_);
        listenSock_ = INVALID_SOCKET;
        return false;
    }

    DWORD timeout = 300;
    setsockopt(listenSock_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));

    running_ = true;
    th_ = std::thread(&ControlServer::loop, this);
    return true;
}

void ControlServer::stop() {
    if (!running_ && !th_.joinable()) return;
    running_ = false;
    if (listenSock_ != INVALID_SOCKET) {
        closesocket(listenSock_);
        listenSock_ = INVALID_SOCKET;
    }
    if (th_.joinable()) th_.join();
}

void ControlServer::loop() {
    while (running_) {
        SOCKET c = accept(listenSock_, nullptr, nullptr);
        if (c == INVALID_SOCKET) continue;

        // 读请求（一次 recv 足够，我们只关心请求行）
        char buf[2048] = {0};
        int n = recv(c, buf, sizeof(buf) - 1, 0);
        if (n <= 0) { closesocket(c); continue; }

        // 解析 "GET /path HTTP/1.1"
        std::string path = "/";
        {
            char method[16] = {0};
            if (sscanf(buf, "%15s %1023s", method, buf + 1024) == 2) {
                path = buf + 1024;
            }
        }

        std::string body;
        std::string ctype = "text/html; charset=utf-8";
        if (path.rfind("/api", 0) == 0) ctype = "application/json; charset=utf-8";

        try {
            body = handler_ ? handler_(path) : std::string("no handler");
        } catch (...) {
            body = "handler error";
        }

        char head[512];
        int headLen = sprintf(head,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %d\r\n"
            "Cache-Control: no-store\r\n"
            "Connection: close\r\n"
            "\r\n",
            ctype.c_str(), (int)body.size());

        int sent = 0;
        while (sent < headLen) {
            int r = send(c, head + sent, headLen - sent, 0);
            if (r <= 0) break;
            sent += r;
        }
        sent = 0;
        while (sent < (int)body.size()) {
            int r = send(c, body.data() + sent, (int)body.size() - sent, 0);
            if (r <= 0) break;
            sent += r;
        }
        closesocket(c);
    }
}

} // namespace ahub
