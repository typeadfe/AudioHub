/*
 * Win11 Fluent 风格主窗口
 *
 * 全部自绘（GDI+ 双缓冲），因为要走"单文件绿色版"路线 ——
 * WinUI 3 需要 .NET + Windows App SDK 运行时，与免安装单 exe 冲突。
 */
#pragma once

#include "player.h"

#include <windows.h>
#include <functional>
#include <string>

namespace uiwin {

struct Options {
    std::string outputDevice;   // WASAPI 输出设备名，显示用
    std::string localIp;        // 本机局域网地址
    uint16_t    port = 53535;
    std::function<bool(bool, std::string&)> toggleReceiver;
    std::function<bool(bool, std::string&)> toggleSender;
    std::function<bool()> requestReceiverScan;
    std::function<bool()> requestSenderScan;
    std::function<bool()> receiverScanning;
    std::function<bool()> senderScanning;
    std::function<int()> receiverState;
    std::function<int()> senderState;
    std::function<std::string()> receiverError;
    std::function<std::string()> senderError;
};

/** 创建并运行主窗口（阻塞直到窗口关闭）。返回进程退出码 */
int run(HINSTANCE hInst, ahub::Player* player, const Options& opt);

} // namespace uiwin
