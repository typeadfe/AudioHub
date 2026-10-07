#pragma once

#include <atomic>
#include <functional>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <cstdint>

namespace ahub {

/** 采集 Windows 默认播放设备的系统混音，并转为 48 kHz 立体声 PCM。 */
class LoopbackSender {
public:
    using FrameHandler = std::function<void(const int16_t*, int)>;
    using ActivePredicate = std::function<bool()>;
    ~LoopbackSender() { stop(); }
    bool start(FrameHandler handler, ActivePredicate active, std::string& err);
    void stop();
    bool isRunning() const { return running_.load(); }

private:
    void loop();
    std::atomic<bool> running_{false};
    std::thread thread_;
    FrameHandler handler_;
    ActivePredicate active_;
    std::mutex startMtx_;
    std::condition_variable startCv_;
    bool started_ = false;
    bool startOk_ = false;
};

} // namespace ahub
