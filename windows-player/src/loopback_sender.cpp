#include "loopback_sender.h"

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <ksmedia.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

namespace ahub {

bool LoopbackSender::start(FrameHandler handler, ActivePredicate active, std::string& err) {
    if (running_) return true;
    if (!handler) { err = "没有音频发送回调"; return false; }
    if (thread_.joinable()) thread_.join();
    handler_ = std::move(handler);
    active_ = std::move(active);
    {
        std::lock_guard<std::mutex> lk(startMtx_);
        started_ = false;
        startOk_ = false;
    }
    running_ = true;
    thread_ = std::thread(&LoopbackSender::loop, this);
    std::unique_lock<std::mutex> lk(startMtx_);
    bool signalled = startCv_.wait_for(lk, std::chrono::seconds(3),
                                       [this] { return started_; });
    bool ok = signalled && startOk_;
    lk.unlock();
    if (!ok) {
        running_ = false;
        if (thread_.joinable()) thread_.join();
        err = "无法采集 Windows 默认播放设备，请检查输出设备";
    }
    return ok;
}

void LoopbackSender::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

static int16_t sample16(const uint8_t* p, int bits, bool floating) {
    if (floating && bits == 32) {
        float f;
        memcpy(&f, p, sizeof(f));
        if (!std::isfinite(f)) f = 0;
        f = std::max(-1.0f, std::min(1.0f, f));
        return (int16_t)(f * 32767.0f);
    }
    if (bits == 16) {
        int16_t v;
        memcpy(&v, p, sizeof(v));
        return v;
    }
    if (bits == 32) {
        int32_t v;
        memcpy(&v, p, sizeof(v));
        return (int16_t)(v >> 16);
    }
    return 0;
}

void LoopbackSender::loop() {
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    IAudioCaptureClient* capture = nullptr;
    WAVEFORMATEX* mix = nullptr;

    do {
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    __uuidof(IMMDeviceEnumerator), (void**)&en))) break;
        if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &device))) break;
        if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                    (void**)&client))) break;
        if (FAILED(client->GetMixFormat(&mix)) || !mix) break;
        bool floating = mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
        if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            auto* ex = (WAVEFORMATEXTENSIBLE*)mix;
            floating = IsEqualGUID(ex->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        }
        const int bits = mix->wBitsPerSample;
        const int channels = mix->nChannels;
        const int rate = (int)mix->nSamplesPerSec;
        if (channels < 1 || rate < 8000 || (bits != 16 && bits != 32)) break;
        if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                      1000000, 0, mix, nullptr))) break;
        if (FAILED(client->GetService(__uuidof(IAudioCaptureClient), (void**)&capture))) break;
        if (FAILED(client->Start())) break;

        {
            std::lock_guard<std::mutex> lk(startMtx_);
            started_ = true;
            startOk_ = true;
        }
        startCv_.notify_one();

        int16_t frame[240 * 2]{};
        int frameFill = 0;
        uint64_t phase = 0;
        bool capturing = true;
        while (running_) {
            if (active_ && !active_()) {
                if (capturing) {
                    client->Stop();
                    client->Reset();
                    capturing = false;
                    frameFill = 0;
                    phase = 0;
                }
                Sleep(250);
                continue;
            }
            if (!capturing) {
                if (FAILED(client->Start())) break;
                capturing = true;
            }
            UINT32 packet = 0;
            if (FAILED(capture->GetNextPacketSize(&packet))) break;
            if (packet == 0) { Sleep(10); continue; }
            BYTE* data = nullptr;
            UINT32 count = 0;
            DWORD flags = 0;
            if (FAILED(capture->GetBuffer(&data, &count, &flags, nullptr, nullptr))) break;
            for (UINT32 i = 0; i < count; i++) {
                int16_t l = 0, r = 0;
                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && data) {
                    const uint8_t* p = data + (size_t)i * mix->nBlockAlign;
                    l = sample16(p, bits, floating);
                    r = channels > 1 ? sample16(p + bits / 8, bits, floating) : l;
                }
                phase += 48000u;
                while (phase >= (uint64_t)rate) {
                    phase -= (uint64_t)rate;
                    frame[frameFill * 2] = l;
                    frame[frameFill * 2 + 1] = r;
                    if (++frameFill == 240) {
                        handler_(frame, 240);
                        frameFill = 0;
                    }
                }
            }
            capture->ReleaseBuffer(count);
        }
        if (capturing) client->Stop();
    } while (false);

    if (mix) CoTaskMemFree(mix);
    if (capture) capture->Release();
    if (client) client->Release();
    if (device) device->Release();
    if (en) en->Release();
    if (SUCCEEDED(co)) CoUninitialize();
    running_ = false;
    {
        std::lock_guard<std::mutex> lk(startMtx_);
        if (!started_) { started_ = true; startOk_ = false; }
    }
    startCv_.notify_one();
}

} // namespace ahub
