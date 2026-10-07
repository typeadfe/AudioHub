#include "wasapi_out.h"
#include "common.h"

#include <avrt.h>
#include <ksmedia.h>
#include <propvarutil.h>
#include <functiondiscoverykeys_devpkey.h>
#include <cmath>
#include <cstring>
#include <future>
#include <cstdio>

// MinGW-w64 8.1 的头文件可能没有这两个标志，补上
#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "avrt.lib")

// MinGW-w64 8.1 不导出 PKEY_Device_FriendlyName 的符号（链接会 undefined reference），
// 这里按 Windows SDK 的定义手动给出：fmtid {a45c254e-df1c-4efd-8020-67d146a850e0}, pid 14
static const PROPERTYKEY kPkeyDeviceFriendlyName = {
    { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } },
    14
};

namespace ahub {

WasapiOut::~WasapiOut() {
    stop();
}

bool WasapiOut::start(PullFn pull, int latencyMs, std::string& err, ActiveFn active) {
    if (running_) return false;
    pull_ = std::move(pull);
    active_ = std::move(active);
    err.clear();

    std::promise<bool> ready;
    auto fut = ready.get_future();
    running_ = true;
    thread_ = std::thread(&WasapiOut::threadProc, this, &ready, &err, latencyMs);

    // 等初始化结果，最多 10 秒
    if (fut.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
        err = "WASAPI 初始化超时";
        running_ = false;
        if (thread_.joinable()) thread_.join();
        return false;
    }
    bool ok = fut.get();
    if (!ok) {
        running_ = false;
        if (thread_.joinable()) thread_.join();
    }
    return ok;
}

void WasapiOut::stop() {
    if (!running_ && !thread_.joinable()) return;
    running_ = false;
    if (event_) SetEvent(event_);   // 唤醒渲染线程
    if (thread_.joinable()) thread_.join();
}

void WasapiOut::threadProc(std::promise<bool>* ready, std::string* errOut, int latencyMs) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;

    auto fail = [&](const std::string& msg) {
        if (errOut && errOut->empty()) *errOut = msg;
        if (render) render->Release();
        if (client) client->Release();
        if (device) device->Release();
        if (enumerator) enumerator->Release();
        if (event_) { CloseHandle(event_); event_ = nullptr; }
        CoUninitialize();
        ready->set_value(false);
    };

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
    if (FAILED(hr) || !enumerator) { fail("CoCreateInstance(MMDeviceEnumerator) 失败"); return; }

    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr) || !device) { fail("取默认播放设备失败（系统可能没有可用输出）"); return; }

    // 记录设备名，便于排查
    {
        IPropertyStore* props = nullptr;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)) && props) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(props->GetValue(kPkeyDeviceFriendlyName, &pv)) && pv.vt == VT_LPWSTR) {
                char buf[512] = {0};
                WideCharToMultiByte(CP_UTF8, 0, pv.pwszVal, -1, buf, sizeof(buf) - 1, nullptr, nullptr);
                deviceName_ = buf;
            }
            PropVariantClear(&pv);
            props->Release();
        }
    }

    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client);
    if (FAILED(hr) || !client) { fail("激活 IAudioClient 失败"); return; }

    WAVEFORMATEX want = {};
    want.wFormatTag      = WAVE_FORMAT_PCM;
    want.nChannels       = (WORD)CHANNELS;
    want.nSamplesPerSec  = SAMPLE_RATE;
    want.wBitsPerSample  = BITS;
    want.nBlockAlign     = (WORD)(CHANNELS * BYTES_PER_SMP);
    want.nAvgBytesPerSec = SAMPLE_RATE * want.nBlockAlign;
    want.cbSize          = 0;

    REFERENCE_TIME dur = (REFERENCE_TIME)latencyMs * 10000;   // 1ms = 10000 * 100ns

    bool autoConvert = true;
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                            AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                            dur, 0, &want, nullptr);
    if (FAILED(hr)) {
        // 退回混音格式
        autoConvert = false;
        client->Release();
        client = nullptr;
        if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client)) || !client) {
            fail("激活 IAudioClient 失败（回退路径）");
            return;
        }
        WAVEFORMATEX* mix = nullptr;
        if (FAILED(client->GetMixFormat(&mix)) || !mix) { fail("GetMixFormat 失败"); return; }
        mixRate_     = mix->nSamplesPerSec;
        mixChannels_ = mix->nChannels;
        mixFloat_    = (mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
                       (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                        ((WAVEFORMATEXTENSIBLE*)mix)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                dur, 0, mix, nullptr);
        CoTaskMemFree(mix);
        if (FAILED(hr)) { fail("IAudioClient::Initialize 失败（两种格式都试过）"); return; }
    }

    UINT32 bufFrames = 0;
    hr = client->GetBufferSize(&bufFrames);
    if (FAILED(hr)) { fail("GetBufferSize 失败"); return; }
    if (bufFrames < (UINT32)(SAMPLE_RATE * latencyMs / 1000)) {
        // 正常现象：共享模式下引擎会向上取整到自己的周期
    }

    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event_) { fail("CreateEvent 失败"); return; }
    hr = client->SetEventHandle(event_);
    if (FAILED(hr)) { fail("SetEventHandle 失败"); return; }

    hr = client->GetService(__uuidof(IAudioRenderClient), (void**)&render);
    if (FAILED(hr) || !render) { fail("取 IAudioRenderClient 失败"); return; }

    // 设备实际格式
    {
        WAVEFORMATEX* cur = nullptr;
        if (SUCCEEDED(client->GetMixFormat(&cur)) && cur) {
            deviceRate_     = cur->nSamplesPerSec;
            deviceChannels_ = cur->nChannels;
            CoTaskMemFree(cur);
        }
    }
    autoConvert_ = autoConvert;

    hr = client->Start();
    if (FAILED(hr)) { fail("IAudioClient::Start 失败"); return; }

    // 提升线程优先级，降低抖动
    DWORD taskIdx = 0;
    HANDLE mmTask = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIdx);

    ready->set_value(true);

    std::vector<int16_t> pullBuf;
    std::vector<int16_t> fifo;              // 48kHz 立体声输入队列
    double readPos = 0.0;                   // 队列内读位置（以输入帧为单位）
    const double inPerOut = (autoConvert || mixRate_ == 0)
                            ? 1.0
                            : (double)SAMPLE_RATE / (double)mixRate_;

    bool outputStarted = true;
    while (running_) {
        if (active_ && !active_()) {
            if (outputStarted) {
                client->Stop();
                client->Reset();
                outputStarted = false;
            }
            Sleep(100);
            continue;
        }
        if (!outputStarted) {
            if (FAILED(client->Start())) break;
            outputStarted = true;
        }
        DWORD w = WaitForSingleObject(event_, 2000);
        if (!running_) break;
        if (w != WAIT_OBJECT_0) continue;

        UINT32 padding = 0;
        if (FAILED(client->GetCurrentPadding(&padding))) continue;
        UINT32 avail = bufFrames - padding;
        if (avail == 0) continue;

        BYTE* data = nullptr;
        if (FAILED(render->GetBuffer(avail, &data))) continue;

        if (autoConvert) {
            pullBuf.resize((size_t)avail * CHANNELS);
            pull_(pullBuf.data(), (int)avail);
            memcpy(data, pullBuf.data(), (size_t)avail * CHANNELS * BYTES_PER_SMP);
        } else if (mixRate_ == SAMPLE_RATE) {
            // 同采样率，只做格式/声道转换
            pullBuf.resize((size_t)avail * CHANNELS);
            pull_(pullBuf.data(), (int)avail);
            convertToMix(data, pullBuf.data(), avail);
        } else {
            // 采样率不同：用输入队列做线性插值
            double needEnd = readPos + (double)avail * inPerOut + 2.0;
            int have = (int)(fifo.size() / CHANNELS);
            if ((double)have < needEnd) {
                int add = (int)std::ceil(needEnd) - have;
                if (add > 0) {
                    size_t old = fifo.size();
                    fifo.resize(old + (size_t)add * CHANNELS);
                    pull_(fifo.data() + old, add);
                }
            }
            have = (int)(fifo.size() / CHANNELS);
            for (UINT32 i = 0; i < avail; i++) {
                int i0 = (int)readPos;
                int i1 = i0 + 1;
                if (i0 >= have) i0 = have - 1;
                if (i1 >= have) i1 = have - 1;
                if (i0 < 0) i0 = 0;
                double frac = readPos - (double)i0;
                for (int c = 0; c < mixChannels_; c++) {
                    double a = (c == 0) ? fifo[(size_t)i0 * CHANNELS + 0]
                             : (c == 1) ? fifo[(size_t)i0 * CHANNELS + 1] : 0.0;
                    double b = (c == 0) ? fifo[(size_t)i1 * CHANNELS + 0]
                             : (c == 1) ? fifo[(size_t)i1 * CHANNELS + 1] : 0.0;
                    double v = a + (b - a) * frac;
                    if (mixFloat_) ((float*)data)[(size_t)i * mixChannels_ + c] = (float)(v / 32768.0);
                    else           ((int16_t*)data)[(size_t)i * mixChannels_ + c] = (int16_t)v;
                }
                readPos += inPerOut;
            }
            int drop = (int)readPos;
            if (drop > 0) {
                if (drop * CHANNELS >= (int)fifo.size()) {
                    fifo.clear();
                    readPos = 0.0;
                } else {
                    fifo.erase(fifo.begin(), fifo.begin() + (size_t)drop * CHANNELS);
                    readPos -= drop;
                }
            }
        }

        render->ReleaseBuffer(avail, 0);
    }

    if (outputStarted) client->Stop();
    if (mmTask) AvRevertMmThreadCharacteristics(mmTask);
    render->Release();
    client->Release();
    device->Release();
    enumerator->Release();
    CloseHandle(event_);
    event_ = nullptr;
    CoUninitialize();
}

void WasapiOut::convertToMix(uint8_t* dst, const int16_t* src, UINT32 frames) {
    const int outCh = mixChannels_ > 0 ? mixChannels_ : 2;
    for (UINT32 i = 0; i < frames; i++) {
        const int16_t l = src[(size_t)i * CHANNELS + 0];
        const int16_t r = src[(size_t)i * CHANNELS + 1];
        for (int c = 0; c < outCh; c++) {
            int16_t v = (c == 0) ? l : (c == 1 ? r : 0);
            if (mixFloat_) ((float*)dst)[(size_t)i * outCh + c] = (float)(v / 32768.0);
            else           ((int16_t*)dst)[(size_t)i * outCh + c] = v;
        }
    }
}

} // namespace ahub
