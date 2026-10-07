/*
 * WASAPI 播放输出
 *
 * 设计：
 *   - 共享模式 + 事件驱动（AUDCLNT_STREAMFLAGS_EVENTCALLBACK）
 *   - 优先用 AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM，让音频引擎替我们做采样率/格式
 *     转换，这样可以固定用 48kHz/立体声/PCM16 送数据，代码里不需要重采样
 *   - 若该标志不被支持，退回混音格式：同采样率只做格式/声道转换；
 *     采样率不同则用输入队列做线性插值重采样
 *   - 所有 WASAPI 对象都在渲染线程内创建/销毁，避免 COM 跨套间问题
 */
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <propidl.h>

#include <cstdint>
#include <functional>
#include <future>
#include <string>
#include <atomic>
#include <thread>

namespace ahub {

class WasapiOut {
public:
    /** 回调：要求填充 frames 帧的交错立体声 int16（48kHz） */
    using PullFn = std::function<void(int16_t* dst, int frames)>;
    using ActiveFn = std::function<bool()>;

    WasapiOut() = default;
    ~WasapiOut();
    WasapiOut(const WasapiOut&) = delete;
    WasapiOut& operator=(const WasapiOut&) = delete;

    /** 启动输出。失败返回 false 并把原因写入 err */
    bool start(PullFn pull, int latencyMs, std::string& err, ActiveFn active = {});
    /** 停止并释放 */
    void stop();

    int  deviceRate()     const { return deviceRate_; }
    int  deviceChannels() const { return deviceChannels_; }
    bool usingAutoConvert() const { return autoConvert_; }
    const std::string& deviceName() const { return deviceName_; }

private:
    void threadProc(std::promise<bool>* ready, std::string* errOut, int latencyMs);
    void convertToMix(uint8_t* dst, const int16_t* src, UINT32 frames);

    PullFn pull_;
    ActiveFn active_;
    HANDLE event_ = nullptr;
    std::thread thread_;
    std::atomic<bool> running_{false};

    volatile int  deviceRate_     = 0;
    volatile int  deviceChannels_ = 0;
    volatile bool autoConvert_    = false;
    std::string   deviceName_;

    // 回退路径（未启用 AUTOCONVERTPCM）时使用
    bool mixFloat_    = false;
    int  mixRate_     = 0;
    int  mixChannels_ = 0;
};

} // namespace ahub
