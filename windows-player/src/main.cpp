/*
 * AudioHub Windows 播放端
 *
 * 运行模式：
 *   ahub-player.exe                        自检：播放 3 秒 440Hz
 *   ahub-player.exe <秒> <频率> [ch]       自检：指定时长/频率；ch 做声道序列测试
 *   ahub-player.exe net [端口] [秒数] [主机]  网络模式
 *
 * 网络模式说明：
 *   手机端是 TCP 服务端，本程序主动连过去 —— 出站连接不受任何防火墙阻挡。
 *   不指定主机时会自动扫描本地网段（出站扫描，同样不受防火墙影响）。
 *   多个主机用逗号分隔，例如： net 53535 0 192.168.1.101,192.168.1.102
 */

// 注意包含顺序：winsock2.h 必须先于 windows.h
#include "player.h"
#include "wasapi_out.h"
#include "control_server.h"
#include "ui_window.h"
#include "ui.h"

#include <atomic>
#include <condition_variable>
#include <cmath>
#include <conio.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <shellapi.h>
#include <string>
#include <thread>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

using namespace ahub;

// ------------------------------------------------------------------
static std::vector<std::string> splitHosts(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t p = s.find(',', start);
        std::string tok = (p == std::string::npos) ? s.substr(start) : s.substr(start, p - start);
        // trim
        size_t a = tok.find_first_not_of(" \t");
        size_t b = tok.find_last_not_of(" \t");
        if (a != std::string::npos) out.push_back(tok.substr(a, b - a + 1));
        if (p == std::string::npos) break;
        start = p + 1;
    }
    return out;
}

// ------------------------------------------------------------------
// 自检模式
// ------------------------------------------------------------------
static int runSelfTest(double seconds, double freq, bool channelTest) {
    printf("=== AudioHub Windows 播放端 —— 自检模式 ===\n");
    printf("音频格式: %d Hz / %d 声道 / %d bit\n", SAMPLE_RATE, CHANNELS, BITS);
    printf("帧长: %d ms (%d 采样/帧, %d 字节/帧)\n", FRAME_MS, FRAME_SAMPLES, FRAME_BYTES);
    printf("音频包: %d 字节（含 %d 字节包头）\n\n", AUDIO_PKT_BYTES, AUDIO_PKT_HEADER);

    if (channelTest) {
        printf("将播放 %.1f 秒声道序列：0-2 秒双声道 / 2-4 秒仅左 / 4-6 秒仅右\n\n", seconds);
    } else {
        printf("将播放 %.1f 秒 %.1f Hz 正弦波\n\n", seconds, freq);
    }

    WasapiOut out;
    std::string err;
    std::atomic<double>    phase{0.0};
    std::atomic<long long> served{0};
    const double step = 2.0 * M_PI * freq / SAMPLE_RATE;
    const double amp  = 0.25 * 32767.0;

    bool ok = out.start([&](int16_t* dst, int frames) {
        double p = phase.load(std::memory_order_relaxed);
        long long base = served.load(std::memory_order_relaxed);
        for (int i = 0; i < frames; i++) {
            int16_t v = (int16_t)(std::sin(p) * amp);
            int16_t l = v, r = v;
            if (channelTest) {
                double t = (double)(base + i) / SAMPLE_RATE;
                if (t < 2.0)      { l = v; r = v; }
                else if (t < 4.0) { l = v; r = 0; }
                else              { l = 0; r = v; }
            }
            dst[(size_t)i * 2 + 0] = l;
            dst[(size_t)i * 2 + 1] = r;
            p += step;
            if (p > 2.0 * M_PI) p -= 2.0 * M_PI;
        }
        phase.store(p, std::memory_order_relaxed);
        served.fetch_add(frames, std::memory_order_relaxed);
    }, 20, err);

    if (!ok) { printf("[失败] 无法启动 WASAPI 输出: %s\n", err.c_str()); return 1; }
    printf("[成功] 输出设备: %s  %d Hz / %d 声道\n\n",
           out.deviceName().c_str(), out.deviceRate(), out.deviceChannels());
    printf(">>> 现在应该能听到声音 <<<\n\n");

    DWORD start = GetTickCount();
    while ((GetTickCount() - start) < (DWORD)(seconds * 1000)) {
        Sleep(200);
        printf("\r  已送出 %.1f / %.1f 秒 ...", (double)served.load() / SAMPLE_RATE, seconds);
        fflush(stdout);
    }
    printf("\n");
    out.stop();
    printf("[完成] 共送出 %.2f 秒音频\n", (double)served.load() / SAMPLE_RATE);
    return 0;
}

// ------------------------------------------------------------------
// 控制界面（浏览器）
// ------------------------------------------------------------------
static const uint16_t CONTROL_PORT = 53536;

static int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static std::string urlDecode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); i++) {
        if (in[i] == '%' && i + 2 < in.size()) {
            int h = hexVal(in[i + 1]), l = hexVal(in[i + 2]);
            if (h >= 0 && l >= 0) { out += (char)(h * 16 + l); i += 2; continue; }
        }
        if (in[i] == '+') { out += ' '; continue; }
        out += in[i];
    }
    return out;
}

/** 从 "/api/set?host=x&gain=1" 里取参数 */
static std::string queryParam(const std::string& path, const std::string& key) {
    size_t q = path.find('?');
    if (q == std::string::npos) return "";
    std::string qs = path.substr(q + 1);
    size_t pos = 0;
    while (pos <= qs.size()) {
        size_t amp = qs.find('&', pos);
        std::string kv = (amp == std::string::npos) ? qs.substr(pos) : qs.substr(pos, amp - pos);
        size_t eq = kv.find('=');
        if (eq != std::string::npos) {
            if (kv.substr(0, eq) == key) return urlDecode(kv.substr(eq + 1));
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return "";
}

static std::string buildStateJson(Player& p) {
    char buf[512];
    std::vector<SourceStat> st = p.stats();
    std::string j = "{\"master\":";
    sprintf(buf, "%.3f", p.masterGain()); j += buf;
    j += ",\"sources\":[";
    for (size_t i = 0; i < st.size(); i++) {
        SourceStat& s = st[i];
        if (i) j += ",";
        j += "{\"host\":\"" + s.host + "\",";
        j += std::string("\"connected\":") + (s.connected ? "true" : "false") + ",";
        sprintf(buf, "\"gain\":%.3f,", s.gain);            j += buf;
        j += std::string("\"muted\":") + (s.muted ? "true" : "false") + ",";
        sprintf(buf, "\"level\":%d,", s.levelPercent);      j += buf;
        sprintf(buf, "\"rmsDb\":%.1f,", s.levelRmsDb);      j += buf;
        sprintf(buf, "\"pendingMs\":%d,", s.pendingMs);     j += buf;
        sprintf(buf, "\"recv\":%llu,", (unsigned long long)s.recv);      j += buf;
        sprintf(buf, "\"lost\":%llu,", (unsigned long long)s.lost);      j += buf;
        sprintf(buf, "\"underruns\":%llu,", (unsigned long long)s.underruns); j += buf;
        sprintf(buf, "\"warmedUp\":%s", s.warmedUp ? "true" : "false");  j += buf;
        j += "}";
    }
    j += "]}";
    return j;
}

static const char* kControlPage = R"HTML(<!DOCTYPE html>
<html lang="zh"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>AudioHub 混音台</title>
<style>
 body{font-family:"Segoe UI","Microsoft YaHei",sans-serif;margin:0;padding:20px;background:#14161a;color:#e6e8eb}
 h1{font-size:18px;margin:0 0 4px}
 .sub{color:#8b94a3;font-size:12px;margin-bottom:16px}
 .card{background:#1c1f26;border-radius:10px;padding:14px 16px;margin-bottom:12px}
 .row{display:flex;align-items:center;gap:12px}
 .dot{width:8px;height:8px;border-radius:50%;background:#4b5563;flex:none}
 .dot.on{background:#22c55e}
 .host{font-family:Consolas,monospace;font-size:14px}
 input[type=range]{flex:1;accent-color:#3b82f6}
 .val{width:52px;text-align:right;font-size:13px;color:#9ca3af}
 button{background:#2a2f38;color:#e6e8eb;border:0;border-radius:6px;padding:6px 12px;cursor:pointer;font-size:13px}
 button.on{background:#ef4444}
 .meter{height:6px;background:#2a2f38;border-radius:3px;overflow:hidden;margin-top:10px}
 .meter>i{display:block;height:100%;width:0;background:linear-gradient(90deg,#22c55e,#eab308,#ef4444)}
 .stats{color:#6b7280;font-size:11px;margin-top:6px;font-family:Consolas,monospace}
</style></head><body>
<h1>AudioHub 混音台</h1>
<div class="sub">拖动滑块调每路音量 · 电平表显示该路原始响度 · 每 0.6 秒刷新</div>
<div id="sources"></div>
<div class="card"><div class="row">
  <span class="dot on"></span><span class="host" style="min-width:60px">主音量</span>
  <input type="range" id="master" min="0" max="200" value="100">
  <span class="val" id="masterVal">100%</span>
</div></div>
<script>
var nodes={},dragging={};
function render(s){
  var box=document.getElementById('sources');
  if(!s.sources.length){
    box.innerHTML='<div class="card">还没有手机接入。<br><span class="sub">在手机上打开 AudioHub 探针点「开始采集」，10 秒内会自动接入。</span></div>';
  } else if(box.firstChild && box.firstChild.className==='card' && box.firstChild.dataset.empty){
    box.removeChild(box.firstChild);
  }
  var mv=document.getElementById('master');
  if(!dragging.master){ mv.value=Math.round(s.master*100); document.getElementById('masterVal').textContent=mv.value+'%'; }
  var seen={};
  for(var i=0;i<s.sources.length;i++){
    var src=s.sources[i]; seen[src.host]=1;
    var n=nodes[src.host];
    if(!n){
      var card=document.createElement('div'); card.className='card';
      card.innerHTML='<div class="row"><span class="dot"></span><span class="host"></span></div>'+
        '<div class="row" style="margin-top:10px"><input type="range" min="0" max="200" value="100">'+
        '<span class="val">100%</span><button>静音</button></div>'+
        '<div class="meter"><i></i></div><div class="stats"></div>';
      box.appendChild(card);
      var sl=card.querySelector('input[type=range]'), bt=card.querySelector('button');
      (function(host,slider,card2){
        slider.addEventListener('pointerdown',function(){dragging[host]=1;});
        slider.addEventListener('input',function(){card2.querySelector('.val').textContent=slider.value+'%';});
        slider.addEventListener('change',function(){dragging[host]=0;setGain(host,slider.value/100);});
      })(src.host,sl,card);
      n=nodes[src.host]={card:card,sl:sl,bt:bt,dot:card.querySelector('.dot'),
        hostEl:card.querySelector('.host'),val:card.querySelector('.val'),
        meter:card.querySelector('.meter>i'),stats:card.querySelector('.stats')};
      (function(host,btn){btn.addEventListener('click',function(){setMute(host,!nodes[host].muted);});})(src.host,bt);
    }
    n.dot.className='dot'+(src.connected?' on':'');
    n.hostEl.textContent=src.host;
    n.muted=src.muted;
    if(!dragging[src.host]){ n.sl.value=Math.round(src.gain*100); n.val.textContent=Math.round(src.gain*100)+'%'; }
    n.bt.textContent=src.muted?'已静音':'静音';
    n.bt.className=src.muted?'on':'';
    n.card.style.opacity=src.muted?0.5:1;
    n.meter.style.width=src.level+'%';
    n.stats.textContent='缓冲 '+src.pendingMs+'ms · 收到 '+src.recv+' · 丢失 '+src.lost+
      ' · 欠载 '+src.underruns+' · '+src.rmsDb.toFixed(1)+' dBFS'+(src.warmedUp?'':' · 正在准备');
  }
  for(var h in nodes){ if(!seen[h]){ nodes[h].card.remove(); delete nodes[h]; } }
}
function tick(){ fetch('/api/state').then(function(r){return r.json();}).then(render).catch(function(){}); }
function setGain(h,g){ fetch('/api/set?host='+encodeURIComponent(h)+'&gain='+g).then(function(r){return r.json();}).then(render).catch(function(){}); }
function setMute(h,m){ fetch('/api/set?host='+encodeURIComponent(h)+'&mute='+(m?1:0)).then(function(r){return r.json();}).then(render).catch(function(){}); }
document.getElementById('master').addEventListener('input',function(e){dragging.master=1;document.getElementById('masterVal').textContent=e.target.value+'%';});
document.getElementById('master').addEventListener('change',function(e){dragging.master=0;
  fetch('/api/set?master='+(e.target.value/100)).then(function(r){return r.json();}).then(render).catch(function(){});});
setInterval(tick,600); tick();
</script></body></html>
)HTML";

static std::string handleControl(Player& player, const std::string& path) {
    if (path.rfind("/api/set", 0) == 0) {
        std::string master = queryParam(path, "master");
        if (!master.empty()) player.setMasterGain((float)atof(master.c_str()));
        std::string host = queryParam(path, "host");
        if (!host.empty()) {
            std::string gain = queryParam(path, "gain");
            if (!gain.empty()) player.setSourceGain(host, (float)atof(gain.c_str()));
            std::string mute = queryParam(path, "mute");
            if (!mute.empty()) player.setSourceMute(host, mute == "1" || mute == "true");
        }
        return buildStateJson(player);
    }
    if (path.rfind("/api/state", 0) == 0) return buildStateJson(player);
    return std::string(kControlPage);
}

// ------------------------------------------------------------------
// 网络模式
// ------------------------------------------------------------------
static int runNetwork(uint16_t port, double seconds, const std::string& hostArg) {
    printf("=== AudioHub Windows 播放端 —— 网络模式 ===\n");
    printf("目标端口: %u（手机端是 TCP 服务端，本程序主动连接）\n\n", port);

    std::vector<std::string> hosts = splitHosts(hostArg);

    DWORD start = GetTickCount();

    std::vector<std::string> mine = Player::localIPv4();
    printf("本机 IPv4: ");
    for (auto& m : mine) printf("%s ", m.c_str());
    printf("\n");

    Player player;
    // 广播用的名字：手机『发送』界面会显示它。
    // 用 Windows 计算机名，用户一眼能认出是哪台机器。
    {
        char cname[MAX_COMPUTERNAME_LENGTH + 1] = {0};
        DWORD n = sizeof(cname);
        if (GetComputerNameA(cname, &n) && n > 0) {
            player.setAnnounceName(std::string(cname, n) + " (PC)");
        } else {
            player.setAnnounceName("Windows PC");
        }
    }
    std::string err;
    if (!player.init(hosts, port, err)) {
        printf("[失败] %s\n", err.c_str());
        return 1;
    }
    if (!player.startReceiverService(err)) {
        printf("[失败] %s\n", err.c_str());
        return 1;
    }
    std::string senderErr;
    if (!player.startSenderService(senderErr))
        printf("[提示] 电脑发送服务未启动: %s\n", senderErr.c_str());
    for (auto& h : hosts) printf("  指定连接 %s:%u\n", h.c_str(), port);
    if (!hosts.empty()) printf("\n");

    WasapiOut out;
    if (!out.start([&](int16_t* dst, int frames) { player.render(dst, frames); }, 20, err)) {
        printf("[失败] 无法启动 WASAPI 输出: %s\n", err.c_str());
        player.shutdown();
        return 1;
    }
    printf("[成功] 输出设备: %s  %d Hz / %d 声道\n\n",
           out.deviceName().c_str(), out.deviceRate(), out.deviceChannels());

    // 控制界面必须【立刻】可用。早期版本把它放在"找到设备之后"才启动，
    // 结果还没接上手机时界面打不开，用户也无从确认服务是否正常。
    ControlServer control;
    std::string ctlErr;
    if (control.start(CONTROL_PORT,
                      [&](const std::string& path) { return handleControl(player, path); },
                      ctlErr)) {
        printf("[成功] 混音台控制界面: http://127.0.0.1:%u/\n", CONTROL_PORT);
        printf("       浏览器打开即可单独调每路音量、单独静音，并看到各路实时电平\n\n");
    } else {
        printf("[警告] 控制界面启动失败: %s（不影响声音）\n\n", ctlErr.c_str());
    }

    printf("等待已指定设备或手机的局域网连接；命令行模式不做周期扫描\n");
    printf("未指定秒数时会一直运行；按 Enter 结束\n\n");

    DWORD lastPrint = 0;
    while (true) {
        Sleep(300);
        if (seconds > 0.0 && (GetTickCount() - start) >= (DWORD)(seconds * 1000)) break;
        if (seconds <= 0.0 && _kbhit()) { _getch(); break; }

        if (GetTickCount() - lastPrint < 3000) continue;
        lastPrint = GetTickCount();

        auto st = player.stats();
        int conn = 0;
        for (auto& s : st) if (s.connected) conn++;

        if (st.empty()) {
            printf("[%5.1fs] 还没有源接入 —— 请在手机上打开 AudioHub 探针并点『开始采集』；"
                   "控制界面: http://127.0.0.1:%u/\n",
                   (GetTickCount() - start) / 1000.0, CONTROL_PORT);
            fflush(stdout);
            continue;
        }

        printf("[%5.1fs] 源=%d 已连接=%d\n",
               (GetTickCount() - start) / 1000.0, (int)st.size(), conn);
        for (auto& s : st) {
            printf("          %-16s %s  %s  %-10s 音量=%3d%%%s  缓冲=%3dms  收=%-7llu 丢=%-6llu 欠载=%-5llu %s\n",
                   s.host.c_str(),
                   s.connected ? "已连接" : "断开  ",
                   s.warmedUp ? "已预热" : "预热中",
                   bwLabel(s.level),
                   (int)(s.gain * 100),
                   s.muted ? " 静音" : "     ",
                   s.pendingMs,
                   (unsigned long long)s.recv,
                   (unsigned long long)s.lost,
                   (unsigned long long)s.underruns,
                   s.err.c_str());
        }
        fflush(stdout);
    }

    control.stop();
    out.stop();
    player.shutdown();
    printf("\n[完成] 已停止\n");
    return 0;
}

// ------------------------------------------------------------------
// 命令行（诊断）模式
//
// 程序本体是 GUI 子系统（无控制台窗口），但保留一个 --console 入口：
// 排查问题时能看到原来那套详细的文字输出，界面版不会让我失去诊断手段。
// ------------------------------------------------------------------
static int runConsoleMode(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "net") {
        uint16_t    port    = (argc > 2) ? (uint16_t)atoi(argv[2]) : DISCOVERY_PORT;
        double      seconds = (argc > 3) ? atof(argv[3]) : 0.0;
        std::string hostArg = (argc > 4) ? argv[4] : "";
        return runNetwork(port, seconds, hostArg);
    }

    double seconds = (argc > 1) ? atof(argv[1]) : 3.0;
    double freq    = (argc > 2) ? atof(argv[2]) : 440.0;
    bool   channelTest = (argc > 3) && (std::string(argv[3]) == "ch");
    if (channelTest && argc <= 1) seconds = 6.0;
    return runSelfTest(seconds, freq, channelTest);
}

// ------------------------------------------------------------------
// 图形界面模式
// ------------------------------------------------------------------
static void applyAnnounceName(Player& p) {
    // 广播与 HELLO 都用这个名字，手机『发送』界面上显示的就是它。
    // 用 Windows 计算机名，用户一眼能认出是哪台机器。
    char cname[MAX_COMPUTERNAME_LENGTH + 1] = {0};
    DWORD n = sizeof(cname);
    if (GetComputerNameA(cname, &n) && n > 0) {
        p.setAnnounceName(std::string(cname, n) + " (PC)");
    } else {
        p.setAnnounceName("Windows PC");
    }
}

static int runGui(HINSTANCE hInst, uint16_t port, const std::string& bindIp) {
    std::string err;

    Player* player = new Player();
    // 默认按 WiFi、有线网、VPN/TUN 的顺序选择；命令行可用 --bind-ip 指定地址。
    player->setPreferredIp(bindIp);
    applyAnnounceName(*player);

    // 允许一开始没有源：靠后台线程周期性扫描动态接入
    if (!player->init(std::vector<std::string>(), port, err)) {
        MessageBoxW(nullptr, (std::wstring(L"AudioHub initialization failed:\n")
                + ui::toWide(err)).c_str(), L"AudioHub", MB_ICONERROR);
        delete player;
        return 1;
    }

    WasapiOut out;
    std::atomic<bool> receiving{false};
    std::atomic<bool> receiverScanning{false};
    std::mutex scanMtx;
    std::condition_variable scanCv;
    bool scanRequested = false;
    std::thread scanner;
    std::thread receiverWorker;
    std::thread senderWorker;
    std::atomic<int> receiverState{0}; // 0 停止、1 启动中、2 运行、3 停止中
    std::atomic<int> senderState{0};
    std::mutex serviceErrorMtx;
    std::string receiverError, senderError;

    uiwin::Options opt;
    opt.outputDevice = "系统默认播放设备";
    opt.port         = port;
    opt.localIp      = player->preferredIp().empty() ? std::string("未知") : player->preferredIp();
    opt.receiverState = [&]() { return receiverState.load(); };
    opt.senderState = [&]() { return senderState.load(); };
    opt.receiverScanning = [&]() { return receiverScanning.load(); };
    opt.senderScanning = [&]() { return player->senderScanning(); };
    opt.requestReceiverScan = [&]() {
        if (receiverState.load() != 2) return false;
        {
            std::lock_guard<std::mutex> lk(scanMtx);
            if (scanRequested || receiverScanning.load()) return false;
            scanRequested = true;
        }
        scanCv.notify_one();
        return true;
    };
    opt.requestSenderScan = [&]() {
        return senderState.load() == 2 && player->requestSenderScan();
    };
    opt.receiverError = [&]() {
        std::lock_guard<std::mutex> lk(serviceErrorMtx);
        return receiverError;
    };
    opt.senderError = [&]() {
        std::lock_guard<std::mutex> lk(serviceErrorMtx);
        return senderError;
    };
    opt.toggleReceiver = [&](bool start, std::string& serviceErr) {
        int expected = start ? 0 : 2;
        if (!receiverState.compare_exchange_strong(expected, start ? 1 : 3)) {
            serviceErr = "服务正在切换状态";
            return false;
        }
        if (receiverWorker.joinable()) receiverWorker.join();
        receiverWorker = std::thread([&, start]() {
            std::string workErr;
            {
                std::lock_guard<std::mutex> lk(serviceErrorMtx);
                receiverError.clear();
            }
        if (start) {
            if (!out.start([player](int16_t* dst, int frames) { player->render(dst, frames); },
                           20, workErr, [player]() { return player->hasRecentAudio(); })) {
                std::lock_guard<std::mutex> lk(serviceErrorMtx);
                receiverError = workErr;
                receiverState = 0;
                return;
            }
            player->setDeviceBufferMs(20);
            if (!player->startReceiverService(workErr)) {
                out.stop();
                std::lock_guard<std::mutex> lk(serviceErrorMtx);
                receiverError = workErr;
                receiverState = 0;
                return;
            }
            receiving = true;
            receiverScanning = false;
            {
                std::lock_guard<std::mutex> lk(scanMtx);
                scanRequested = false;
            }
            scanner = std::thread([&]() {
                while (receiving.load()) {
                    {
                        std::unique_lock<std::mutex> lk(scanMtx);
                        scanCv.wait(lk, [&]() { return !receiving.load() || scanRequested; });
                        if (!receiving.load()) break;
                        scanRequested = false;
                        receiverScanning = true;
                    }
                    int scanned = 0;
                    std::vector<std::string> found = player->scanNew(port, 250, &scanned, &receiving);
                    for (const auto& h : found) {
                        if (!receiving.load()) break;
                        player->addHost(h, port);
                    }
                    receiverScanning = false;
                }
            });
            receiverState = 2;
        } else {
            receiving = false;
            scanCv.notify_all();
            if (scanner.joinable()) scanner.join();
            out.stop();
            player->stopReceiverService();
            receiverState = 0;
        }
        });
        return true;
    };
    opt.toggleSender = [&](bool start, std::string& serviceErr) {
        int expected = start ? 0 : 2;
        if (!senderState.compare_exchange_strong(expected, start ? 1 : 3)) {
            serviceErr = "服务正在切换状态";
            return false;
        }
        if (senderWorker.joinable()) senderWorker.join();
        senderWorker = std::thread([&, start]() {
            std::string workErr;
            {
                std::lock_guard<std::mutex> lk(serviceErrorMtx);
                senderError.clear();
            }
            if (start) {
                if (!player->startSenderService(workErr)) {
                    std::lock_guard<std::mutex> lk(serviceErrorMtx);
                    senderError = workErr;
                    senderState = 0;
                    return;
                }
                if (!player->startAudioSender(workErr)) {
                    std::lock_guard<std::mutex> lk(serviceErrorMtx);
                    senderError = "服务已启动，但声音采集失败：" + workErr;
                }
                senderState = 2;
            } else {
                player->stopSenderService();
                senderState = 0;
            }
        });
        return true;
    };

    int rc = uiwin::run(hInst, player, opt);

    if (receiverWorker.joinable()) receiverWorker.join();
    if (senderWorker.joinable()) senderWorker.join();
    receiving = false;
    scanCv.notify_all();
    if (scanner.joinable()) scanner.join();
    out.stop();
    player->shutdown();
    delete player;
    return rc;
}

// ------------------------------------------------------------------
// 入口。GUI 子系统下没有控制台，--console 时附加到父控制台。
// ------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    int argcW = 0;
    LPWSTR* argvW = CommandLineToArgvW(GetCommandLineW(), &argcW);

    std::vector<std::string> args;
    bool consoleMode = false;
    uint16_t port = DISCOVERY_PORT;
    std::string bindIp;

    for (int i = 1; i < argcW; i++) {
        std::wstring w(argvW[i]);
        // 宽字符转 UTF-8
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                    nullptr, 0, nullptr, nullptr);
        std::string a((size_t)(n > 0 ? n : 0), '\0');
        if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                       &a[0], n, nullptr, nullptr);

        if (a == "--console" || a == "-c") { consoleMode = true; continue; }
        if (a == "--port" && i + 1 < argcW) {
            std::wstring w2(argvW[++i]);
            port = (uint16_t)_wtoi(w2.c_str());
            continue;
        }
        if (a == "--bind-ip" && i + 1 < argcW) {
            std::wstring w2(argvW[++i]);
            int n2 = WideCharToMultiByte(CP_UTF8, 0, w2.c_str(), (int)w2.size(),
                                         nullptr, 0, nullptr, nullptr);
            bindIp.assign((size_t)(n2 > 0 ? n2 : 0), '\0');
            if (n2 > 0) WideCharToMultiByte(CP_UTF8, 0, w2.c_str(), (int)w2.size(),
                                            &bindIp[0], n2, nullptr, nullptr);
            continue;
        }
        args.push_back(a);
    }
    if (argvW) LocalFree(argvW);

    if (consoleMode) {
        // 附加到启动本程序的父控制台（从 cmd/PowerShell 里跑才有）
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            FILE* f = nullptr;
            freopen_s(&f, "CONOUT$", "w", stdout);
            freopen_s(&f, "CONOUT$", "w", stderr);
            freopen_s(&f, "CONIN$",  "r", stdin);
        }
        std::vector<char*> argv;
        argv.push_back((char*)"ahub-player");
        for (auto& s : args) argv.push_back((char*)s.c_str());
        return runConsoleMode((int)argv.size(), argv.data());
    }

    return runGui(hInst, port ? port : DISCOVERY_PORT, bindIp);
}
