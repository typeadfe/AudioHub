/*
 * 极简 HTTP 控制服务（仅监听 127.0.0.1）
 *
 * 为什么需要它：播放端是控制台程序，做不了滑块。而"每路音量"必须能实时调、
 * 还要能看见哪一路本来就偏响 —— 所以给一个浏览器界面是最合适的。
 *
 * 只支持 GET，只绑定回环地址（不对外暴露），够用即可。
 */
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace ahub {

class ControlServer {
public:
    /** 传入请求路径（含 query），返回响应体；以 /api 开头时按 JSON 返回 */
    using Handler = std::function<std::string(const std::string& path)>;

    ~ControlServer();
    ControlServer() = default;
    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;

    bool start(uint16_t port, Handler handler, std::string& err);
    void stop();

private:
    void loop();

    SOCKET            listenSock_ = INVALID_SOCKET;
    std::thread       th_;
    std::atomic<bool> running_{false};
    Handler           handler_;
    uint16_t          port_ = 0;
};

} // namespace ahub
