// SetProcessDPIAware 等 API 在 winuser.h 里受 WINVER 版本宏保护，
// 必须在任何头文件之前定义，否则 MinGW 不会声明它们。
#ifndef WINVER
#define WINVER 0x0601
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include "ui_window.h"
#include "ui.h"

#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <algorithm>
#include <cstring>
#include <vector>

using namespace Gdiplus;

namespace uiwin {
namespace {

// ==================================================================
// Win11 DWM 属性。老版本 SDK 的头文件里没有这些常量，
// 自行定义即可 —— 系统不支持时 DwmSetWindowAttribute 会直接返回失败，
// 不会造成任何问题，窗口只是没有圆角。
// ==================================================================
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif

// ==================================================================
// 布局常量（逻辑像素，实际绘制走 ui::scale 做 DPI 缩放）
// ==================================================================
constexpr int TITLE_H   = 46;
constexpr int MODE_H    = 56;
constexpr int SERVICE_H = 128;
constexpr int MASTER_H  = 92;
constexpr int STATUS_H  = 30;
constexpr int CARD_H    = 112;
constexpr int CARD_GAP  = 10;
constexpr int PAD       = 16;
constexpr int RESIZE_M  = 6;      // 可拖拽改变大小的边框宽度

struct CardHit {
    std::string host;
    RectF card;     // 整张卡
    RectF slider;   // 音量滑块
    RectF mute;     // 静音按钮
};

struct PeerHit {
    std::string address;
    RectF card;
    RectF toggle;
};

struct App {
    HWND          hwnd   = nullptr;
    ahub::Player* player = nullptr;
    Options       opt;

    // 双缓冲
    HDC     memDC  = nullptr;
    HBITMAP memBmp = nullptr;
    int     memW = 0, memH = 0;

    std::vector<CardHit> hits;
    std::vector<PeerHit> peerHits;
    float scrollY   = 0;
    float maxScroll = 0;

    // 交互状态
    int  dragSlider  = -1;     // 正在拖动的卡片索引
    int  hoverSlider = -1;
    int  hoverMute   = -1;
    int  hoverPeer = -1;
    bool hoverMaster = false;
    bool hoverClipboardSend = false;
    bool hoverReceiverClipboard = false;
    bool hoverReceiverScan = false;
    bool hoverSenderScan = false;
    bool hoverReceiverService = false;
    bool hoverSenderService = false;
    bool senderMode = false;
    std::wstring actionNotice;
    DWORD noticeUntil = 0;
    uint64_t lastClipboardSeq = 0;
    int lastReceiverState = 0;
    int lastSenderState = 0;
    bool dragMaster  = false;
    bool hoverClose  = false;
    bool hoverMin    = false;
    bool tracking    = false;
    bool trayAdded   = false;
    bool trayHintShown = false;
    HICON trayIcon = nullptr;
    bool ownsTrayIcon = false;

    int  exitCode = 0;
};

static App g;
static UINT taskbarCreated = 0;
constexpr UINT TRAY_MESSAGE = WM_APP + 1;
constexpr UINT TRAY_MINIMIZE = WM_APP + 2;
constexpr UINT TRAY_ID = 1;
constexpr UINT TRAY_OPEN = 1001;
constexpr UINT TRAY_EXIT = 1002;

NOTIFYICONDATAW trayData(HWND hwnd) {
    NOTIFYICONDATAW data = {};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd;
    data.uID = TRAY_ID;
    return data;
}

bool addTrayIcon(App& a) {
    if (a.trayAdded) return true;
    NOTIFYICONDATAW data = trayData(a.hwnd);
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = TRAY_MESSAGE;
    if (!a.trayIcon) {
        a.trayIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101),
                                       IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                       GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
        a.ownsTrayIcon = a.trayIcon != nullptr;
        if (!a.trayIcon) a.trayIcon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    data.hIcon = a.trayIcon;
    lstrcpynW(data.szTip, L"AudioHub", ARRAYSIZE(data.szTip));
    a.trayAdded = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
    return a.trayAdded;
}

void removeTrayIcon(App& a) {
    if (a.trayAdded) {
        NOTIFYICONDATAW data = trayData(a.hwnd);
        Shell_NotifyIconW(NIM_DELETE, &data);
        a.trayAdded = false;
    }
    if (a.ownsTrayIcon) DestroyIcon(a.trayIcon);
    a.trayIcon = nullptr;
    a.ownsTrayIcon = false;
}

void trayNotice(App& a, const std::wstring& message) {
    if (!a.trayAdded) return;
    NOTIFYICONDATAW data = trayData(a.hwnd);
    data.uFlags = NIF_INFO;
    data.dwInfoFlags = NIIF_INFO;
    lstrcpynW(data.szInfoTitle, L"AudioHub", ARRAYSIZE(data.szInfoTitle));
    lstrcpynW(data.szInfo, message.c_str(), ARRAYSIZE(data.szInfo));
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void minimizeToTray(App& a) {
    ShowWindow(a.hwnd, SW_HIDE);
    KillTimer(a.hwnd, 1);
    SetTimer(a.hwnd, 1, 1000, nullptr);
    if (!a.trayHintShown) {
        trayNotice(a, L"已最小化到托盘，双击图标可打开窗口");
        a.trayHintShown = true;
    }
}

void restoreFromTray(App& a) {
    ShowWindow(a.hwnd, SW_RESTORE);
    KillTimer(a.hwnd, 1);
    SetTimer(a.hwnd, 1, 250, nullptr);
    SetForegroundWindow(a.hwnd);
    InvalidateRect(a.hwnd, nullptr, FALSE);
}

void showTrayMenu(App& a) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, TRAY_OPEN, L"打开 AudioHub");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, TRAY_EXIT, L"退出 AudioHub");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(a.hwnd);
    UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                  pt.x, pt.y, 0, a.hwnd, nullptr);
    PostMessageW(a.hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (command == TRAY_OPEN) restoreFromTray(a);
    else if (command == TRAY_EXIT) DestroyWindow(a.hwnd);
}

// ==================================================================
// 布局
// ==================================================================
void layout(App& a, int W, int H) {
    a.hits.clear();
    a.peerHits.clear();
    if (a.senderMode) {
        std::vector<ahub::ClipboardServer::PeerStat> peers = a.player->senderPeers();
        const int top = ui::scale(TITLE_H + MODE_H + 252);
        const int bottom = H - ui::scale(STATUS_H);
        const int cardH = ui::scale(76);
        const int gap = ui::scale(8);
        a.maxScroll = std::max(0.0f, (float)(peers.size() * (cardH + gap) - (bottom - top)));
        a.scrollY = std::max(0.0f, std::min(a.scrollY, a.maxScroll));
        float y = (float)top - a.scrollY;
        const int pad = ui::scale(PAD);
        for (const auto& peer : peers) {
            PeerHit hit;
            hit.address = peer.address;
            hit.card = RectF((REAL)pad, y, (REAL)(W - pad * 2), (REAL)cardH);
            hit.toggle = RectF((REAL)(W - pad - ui::scale(70)),
                               y + ui::scale(24),
                               (REAL)ui::scale(52), (REAL)ui::scale(28));
            a.peerHits.push_back(hit);
            y += cardH + gap;
        }
        return;
    }

    std::vector<ahub::SourceStat> st = a.player->stats();

    const int pad    = ui::scale(PAD);
    const int cardH  = ui::scale(CARD_H);
    const int gap    = ui::scale(CARD_GAP);
    const int top    = ui::scale(TITLE_H + MODE_H + SERVICE_H + MASTER_H);
    const int bottom = H - ui::scale(STATUS_H);

    float total = (float)st.size() * (float)(cardH + gap);
    float view  = (float)(bottom - top);
    a.maxScroll = std::max(0.0f, total - view);
    if (a.scrollY > a.maxScroll) a.scrollY = a.maxScroll;
    if (a.scrollY < 0) a.scrollY = 0;

    float y = (float)top - a.scrollY;
    for (auto& s : st) {
        const int innerL  = pad + ui::scale(32);
        const int innerR  = W - pad - ui::scale(16);
        const int innerW  = innerR - innerL;
        const int rowY    = (int)y + ui::scale(74);
        const int muteW   = ui::scale(64);
        const int gainW   = ui::scale(56);
        const int sliderW = std::max(ui::scale(60),
                                     innerW - muteW - gainW - ui::scale(24));

        CardHit h;
        h.host   = s.host;
        h.card   = RectF((REAL)pad, (REAL)y, (REAL)(W - pad * 2), (REAL)cardH);
        h.slider = RectF((REAL)innerL, (REAL)rowY, (REAL)sliderW, (REAL)ui::scale(28));
        h.mute   = RectF((REAL)(innerR - muteW), (REAL)rowY,
                         (REAL)muteW, (REAL)ui::scale(28));
        a.hits.push_back(h);

        y += (float)(cardH + gap);
    }
}

/** 主音量滑块矩形 */
RectF masterRect(int W) {
    const int pad = ui::scale(PAD);
    return RectF((REAL)pad, (REAL)ui::scale(TITLE_H + MODE_H + SERVICE_H + 44),
                 (REAL)(W - pad * 2), (REAL)ui::scale(26));
}

/** 标题栏按钮矩形（最小化 / 关闭），按 Win11 的 46x32 规格 */
RectF titleBtn(int W, bool close) {
    const int bw = ui::scale(46);
    const int bh = ui::scale(32);
    int y = (ui::scale(TITLE_H) - bh) / 2;
    int x = close ? (W - bw) : (W - bw * 2);
    return RectF((REAL)x, (REAL)y, (REAL)bw, (REAL)bh);
}

// ==================================================================
// 各区域绘制
// ==================================================================
RectF modeRect(int W, bool sender);
void paintTitleBar(App& a, Graphics& gr, int W) {
    const int th = ui::scale(TITLE_H);

    // 左侧强调色小圆点 + 标题
    ui::circle(gr, (REAL)ui::scale(24), (REAL)(th / 2), (REAL)ui::scale(5),
               ui::color::accent());
    ui::text(gr, L"AudioHub",
             RectF((REAL)ui::scale(38), 0, (REAL)(W / 2), (REAL)th),
             ui::fontTitle(), ui::color::text(), 0);

    // 右侧按钮（自绘，避免非客户区与自绘风格不一致）
    struct Btn { RectF r; bool close; bool hover; };
    Btn btns[2] = {
        { titleBtn(W, false), false, a.hoverMin },
        { titleBtn(W, true),  true,  a.hoverClose },
    };
    for (auto& b : btns) {
        if (b.hover) {
            Color bg = b.close ? ui::color::danger() : ui::color::layerHover();
            ui::roundedRect(gr, b.r, (REAL)ui::scale(6), bg, nullptr);
        }
        Color fg = (b.hover && b.close) ? ui::color::text() : ui::color::textSecond();
        float cx = b.r.X + b.r.Width / 2;
        float cy = b.r.Y + b.r.Height / 2;
        Pen pen(fg, (REAL)ui::scale(1));
        float s = (REAL)ui::scale(5);
        if (b.close) {
            gr.DrawLine(&pen, cx - s, cy - s, cx + s, cy + s);
            gr.DrawLine(&pen, cx + s, cy - s, cx - s, cy + s);
        } else {
            gr.DrawLine(&pen, cx - s, cy, cx + s, cy);
        }
    }
}

void paintMode(App& a, Graphics& gr, int W) {
    for (int i = 0; i < 2; ++i) {
        bool sender = i == 1;
        RectF r = modeRect(W, sender);
        bool selected = a.senderMode == sender;
        Color stroke = selected ? ui::color::accent() : ui::color::stroke();
        ui::roundedRect(gr, r, (REAL)ui::scale(9),
                        selected ? ui::color::accentDim() : ui::color::layer(),
                        &stroke, 1.0f);
        ui::text(gr, sender ? L"发送" : L"接收", r, ui::fontBodyStrong(),
                 selected ? ui::color::text() : ui::color::textSecond(), 1);
    }
}

void paintMaster(App& a, Graphics& gr, int W) {
    const int pad = ui::scale(PAD);
    const int y0  = ui::scale(TITLE_H + MODE_H + SERVICE_H);

    // 分隔线
    Pen sep(ui::color::strokeSoft(), 1.0f);
    gr.DrawLine(&sep, (REAL)pad, (REAL)y0, (REAL)(W - pad), (REAL)y0);

    std::vector<ahub::SourceStat> st = a.player->stats();
    int connected = 0;
    for (auto& s : st) if (s.connected) connected++;

    ui::text(gr, L"主音量",
             RectF((REAL)pad, (REAL)(y0 + ui::scale(8)), (REAL)ui::scale(120), (REAL)ui::scale(24)),
             ui::fontBodyStrong(), ui::color::text(), 0);

    wchar_t buf[64];
    swprintf(buf, 64, L"%d%%", (int)(a.player->masterGain() * 100));
    ui::text(gr, buf,
             RectF((REAL)(W - pad - ui::scale(90)), (REAL)(y0 + ui::scale(8)),
                   (REAL)ui::scale(90), (REAL)ui::scale(24)),
             ui::fontBody(), ui::color::textSecond(), 2);

    ui::slider(gr, masterRect(W), a.player->masterGain(), a.hoverMaster, a.dragMaster);

    // 输出设备与连接数
    std::string sub = "输出: " + a.opt.outputDevice;
    ui::text(gr, ui::toWide(sub),
             RectF((REAL)pad, (REAL)(y0 + ui::scale(70)), (REAL)(W - pad * 2 - ui::scale(110)),
                   (REAL)ui::scale(18)),
             ui::fontCaption(), ui::color::textTertiary(), 0);

    swprintf(buf, 64, L"已连接 %d 路", connected);
    ui::text(gr, buf,
             RectF((REAL)(W - pad - ui::scale(110)), (REAL)(y0 + ui::scale(70)),
                   (REAL)ui::scale(110), (REAL)ui::scale(18)),
             ui::fontCaption(), connected > 0 ? ui::color::ok() : ui::color::textTertiary(), 2);
}

RectF clipboardSendRect(int W) {
    const int pad = ui::scale(PAD);
    return RectF((REAL)(pad + ui::scale(14)),
                 (REAL)ui::scale(TITLE_H + MODE_H + 176),
                 (REAL)(W - pad * 2 - ui::scale(28)), (REAL)ui::scale(38));
}

RectF modeRect(int W, bool sender) {
    const int pad = ui::scale(PAD);
    const int gap = ui::scale(8);
    const int half = (W - pad * 2 - gap) / 2;
    return RectF((REAL)(pad + (sender ? half + gap : 0)),
                 (REAL)ui::scale(TITLE_H + 8), (REAL)half, (REAL)ui::scale(40));
}

RectF receiverServiceRect(int W) {
    const int pad = ui::scale(PAD);
    const int bw = (W - pad * 2 - ui::scale(44)) / 3;
    return RectF((REAL)(pad + ui::scale(14)),
                 (REAL)ui::scale(TITLE_H + MODE_H + 65),
                 (REAL)bw, (REAL)ui::scale(38));
}

RectF receiverScanRect(int W) {
    RectF left = receiverServiceRect(W);
    return RectF(left.GetRight() + ui::scale(8), left.Y,
                 left.Width, left.Height);
}

RectF receiverClipboardRect(int W) {
    RectF middle = receiverScanRect(W);
    return RectF(middle.GetRight() + ui::scale(8), middle.Y,
                 middle.Width, middle.Height);
}

RectF senderServiceRect(int W) {
    const int pad = ui::scale(PAD);
    return RectF((REAL)(W - pad - ui::scale(164)),
                 (REAL)ui::scale(TITLE_H + MODE_H + 130),
                 (REAL)ui::scale(150), (REAL)ui::scale(38));
}

RectF senderScanRect(int W) {
    RectF right = senderServiceRect(W);
    const int left = ui::scale(PAD + 14);
    return RectF((REAL)left, right.Y,
                 right.X - left - ui::scale(10), right.Height);
}

void paintReceiverService(App& a, Graphics& gr, int W) {
    const int pad = ui::scale(PAD);
    RectF card((REAL)pad, (REAL)ui::scale(TITLE_H + MODE_H + 8),
               (REAL)(W - pad * 2), (REAL)ui::scale(112));
    Color stroke = ui::color::stroke();
    ui::roundedRect(gr, card, (REAL)ui::scale(10), ui::color::layerAlt(), &stroke, 1.0f);
    int state = a.opt.receiverState ? a.opt.receiverState() : 0;
    bool running = state == 2;
    ui::text(gr, L"接收手机声音",
             RectF(card.X + ui::scale(14), card.Y + ui::scale(8),
                   card.Width - ui::scale(28), (REAL)ui::scale(25)),
             ui::fontBodyStrong(), ui::color::text(), 0);
    const wchar_t* sub = state == 1 ? L"正在启动接收服务" : state == 3 ? L"正在停止接收服务"
                       : running ? L"等待设备连接；需要时点击扫描" : L"开始接收后可手动扫描设备";
    ui::text(gr, sub,
             RectF(card.X + ui::scale(14), card.Y + ui::scale(37),
                   card.Width - ui::scale(28), (REAL)ui::scale(22)),
             ui::fontCaption(), running ? ui::color::ok() : ui::color::textTertiary(), 0);
    ui::outlineButton(gr, receiverServiceRect(W), state == 1 ? L"启动中" : state == 3 ? L"停止中"
                      : running ? L"停止接收" : L"开始接收",
                      ui::fontCaption(), a.hoverReceiverService, running);
    ui::outlineButton(gr, receiverScanRect(W),
                      a.opt.receiverScanning && a.opt.receiverScanning() ? L"扫描中" : L"扫描设备",
                      ui::fontCaption(), a.hoverReceiverScan, false);
    ui::outlineButton(gr, receiverClipboardRect(W), L"发送剪贴板",
                      ui::fontCaption(), a.hoverReceiverClipboard, false);
}

void paintSender(App& a, Graphics& gr, int W) {
    const int pad = ui::scale(PAD);
    const int y = ui::scale(TITLE_H + MODE_H + 8);
    RectF card((REAL)pad, (REAL)y, (REAL)(W - pad * 2), (REAL)ui::scale(220));
    Color stroke = ui::color::stroke();
    ui::roundedRect(gr, card, (REAL)ui::scale(10), ui::color::layerAlt(), &stroke, 1.0f);
    ui::text(gr, L"电脑声音与剪贴板",
             RectF(card.X + ui::scale(14), card.Y + ui::scale(10),
                   card.Width - ui::scale(28), (REAL)ui::scale(26)),
             ui::fontBodyStrong(), ui::color::text(), 0);
    int peers = a.player->clipboardPeerCount();
    int serviceState = a.opt.senderState ? a.opt.senderState() : 0;
    std::wstring state;
    Color stateColor = ui::color::textSecond();
    if (serviceState != 2) {
        std::string error = a.opt.senderError ? a.opt.senderError() : "";
        state = serviceState == 1 ? L"正在启动发送服务" : serviceState == 3 ? L"正在停止发送服务"
              : error.empty() ? L"尚未开始发送" : L"启动失败：" + ui::toWide(error);
        stateColor = error.empty() ? ui::color::textSecond() : ui::color::danger();
    } else if (peers == 0) {
        state = a.player->isAudioSenderRunning() ? L"声音已开启，等待手机接收端连接" : L"等待手机接收端连接";
    } else {
        state = L"已连接 " + std::to_wstring(peers) + L" 台接收设备";
        stateColor = ui::color::ok();
    }
    ui::circle(gr, card.X + ui::scale(20), card.Y + ui::scale(52),
               (REAL)ui::scale(5), stateColor);
    ui::text(gr, state,
             RectF(card.X + ui::scale(32), card.Y + ui::scale(39),
                   card.Width - ui::scale(46), (REAL)ui::scale(27)),
             ui::fontBody(), stateColor, 0);
    std::string addr = a.opt.localIp + ":" + std::to_string(ahub::DISCOVERY_PORT);
    ui::text(gr, ui::toWide("电脑地址 " + addr),
             RectF(card.X + ui::scale(14), card.Y + ui::scale(70),
                   card.Width - ui::scale(28), (REAL)ui::scale(20)),
             ui::fontCaption(), ui::color::textSecond(), 0);
    std::vector<std::string> addrs = a.player->clipboardPeerAddresses();
    std::wstring detail = addrs.empty()
        ? L"在手机「接收」页点开始，再点击扫描设备"
        : L"接收设备 " + ui::toWide(addrs.front()) + (addrs.size() > 1 ? L" 等" : L"");
    ui::text(gr, detail,
             RectF(card.X + ui::scale(14), card.Y + ui::scale(96),
                   card.Width - ui::scale(28), (REAL)ui::scale(20)),
             ui::fontCaption(), ui::color::textTertiary(), 0);
    ui::outlineButton(gr, senderServiceRect(W),
                      serviceState == 1 ? L"启动中" : serviceState == 3 ? L"停止中"
                        : serviceState == 2 ? L"停止发送" : L"开始发送",
                      ui::fontCaption(), a.hoverSenderService,
                      serviceState == 2);
    ui::outlineButton(gr, senderScanRect(W),
                      a.opt.senderScanning && a.opt.senderScanning() ? L"扫描中" : L"扫描设备",
                      ui::fontCaption(), a.hoverSenderScan, false);
    ui::outlineButton(gr, clipboardSendRect(W), L"发送剪贴板",
                      ui::fontCaption(), a.hoverClipboardSend, false);
    ui::text(gr, L"已连接的接收设备",
             RectF((REAL)pad, (REAL)ui::scale(TITLE_H + MODE_H + 230),
                   (REAL)(W - pad * 2), (REAL)ui::scale(20)),
             ui::fontBodyStrong(), ui::color::text(), 0);
}

void paintSenderPeers(App& a, Graphics& gr, int W, int H) {
    std::vector<ahub::ClipboardServer::PeerStat> peers = a.player->senderPeers();
    if (peers.empty()) {
        ui::text(gr, L"暂无接收设备。请在手机「接收」页点击开始。",
                 RectF(0, (REAL)ui::scale(TITLE_H + MODE_H + 278),
                       (REAL)W, (REAL)ui::scale(30)),
                 ui::fontCaption(), ui::color::textTertiary(), 1);
        return;
    }
    const int listTop = ui::scale(TITLE_H + MODE_H + 252);
    const int listBottom = H - ui::scale(STATUS_H);
    for (size_t i = 0; i < a.peerHits.size(); ++i) {
        const PeerHit& hit = a.peerHits[i];
        if (hit.card.GetBottom() < listTop || hit.card.Y > listBottom) continue;
        auto it = std::find_if(peers.begin(), peers.end(), [&](const auto& p) {
            return p.address == hit.address;
        });
        if (it == peers.end()) continue;
        Color stroke = ui::color::stroke();
        ui::roundedRect(gr, hit.card, (REAL)ui::scale(8), ui::color::layer(), &stroke, 1.0f);
        ui::circle(gr, hit.card.X + ui::scale(18), hit.card.Y + ui::scale(21),
                   (REAL)ui::scale(5), it->audioEnabled ? ui::color::ok() : ui::color::idle());
        ui::text(gr, ui::toWide(it->name),
                 RectF(hit.card.X + ui::scale(32), hit.card.Y + ui::scale(8),
                       hit.card.Width - ui::scale(120), (REAL)ui::scale(24)),
                 ui::fontBodyStrong(), ui::color::text(), 0);
        std::string subtitle = it->address + " · "
                + (it->latencyMs > 0 ? "延迟约 " + std::to_string(it->latencyMs) + " ms"
                                      : "延迟测量中");
        ui::text(gr, ui::toWide(subtitle),
                 RectF(hit.card.X + ui::scale(32), hit.card.Y + ui::scale(38),
                       hit.card.Width - ui::scale(130), (REAL)ui::scale(22)),
                 ui::fontCaption(), ui::color::textSecond(), 0);
        ui::toggle(gr, hit.toggle, it->audioEnabled, a.hoverPeer == (int)i);
    }
}

void paintCards(App& a, Graphics& gr, int W, int H) {
    std::vector<ahub::SourceStat> st = a.player->stats();
    if (st.empty()) {
        ui::text(gr, L"还没有音源接入",
                 RectF(0, (REAL)(ui::scale(TITLE_H + MODE_H + SERVICE_H + MASTER_H)), (REAL)W, (REAL)ui::scale(60)),
                 ui::fontBodyStrong(), ui::color::textSecond(), 1);
        ui::text(gr, L"在手机上打开 AudioHub，切到「发送」并点开始",
                 RectF(0, (REAL)(ui::scale(TITLE_H + MODE_H + SERVICE_H + MASTER_H + 34)), (REAL)W, (REAL)ui::scale(24)),
                 ui::fontCaption(), ui::color::textTertiary(), 1);
        return;
    }

    const int cardsTop = ui::scale(TITLE_H + MODE_H + SERVICE_H + MASTER_H);
    const int cardsBot = H - ui::scale(STATUS_H);

    for (size_t i = 0; i < st.size() && i < a.hits.size(); i++) {
        const ahub::SourceStat& s = st[i];
        const CardHit& hit = a.hits[i];

        // 只画可见部分（卡很多时列表可滚动）
        if (hit.card.GetBottom() < cardsTop || hit.card.Y > cardsBot) continue;

        const bool pending = s.pendingApproval;
        const bool bad = s.denied;

        // 卡片底
        Color strokeC = ui::color::stroke();
        ui::roundedRect(gr, hit.card, (REAL)ui::scale(8), ui::color::layer(),
                        &strokeC, 1.0f);

        // 状态圆点
        Color dotC = bad ? ui::color::danger()
                   : pending ? ui::color::warn()
                   : (s.connected ? ui::color::ok() : ui::color::idle());
        ui::circle(gr, hit.card.X + ui::scale(18), hit.card.Y + ui::scale(22),
                   (REAL)ui::scale(5), dotC);

        const REAL innerL = hit.card.X + ui::scale(32);
        const REAL innerR = hit.card.GetRight() - ui::scale(16);
        const REAL innerW = innerR - innerL;

        // 设备名
        std::string nm = s.name.empty() ? s.host : s.name;
        ui::text(gr, ui::toWide(nm),
                 RectF(innerL, hit.card.Y + ui::scale(8),
                       innerW - ui::scale(110), (REAL)ui::scale(24)),
                 ui::fontBodyStrong(), ui::color::text(), 0);

        // 右上角：延迟 / 状态
        wchar_t rbuf[96];
        if (pending)        swprintf(rbuf, 96, L"等待允许");
        else if (s.denied)  swprintf(rbuf, 96, L"已被拒绝");
        else if (!s.connected) swprintf(rbuf, 96, L"未连接");
        else if (!s.recentAudio) swprintf(rbuf, 96, L"已连接待发送");
        else if (s.pendingMs > 0) swprintf(rbuf, 96, L"延迟 %d ms", s.pendingMs);
        else                swprintf(rbuf, 96, L"接收中");
        Color rc = pending ? ui::color::warn()
                 : bad ? ui::color::danger()
                 : (s.connected ? ui::color::ok() : ui::color::textSecond());
        ui::text(gr, rbuf,
                 RectF(innerR - ui::scale(110), hit.card.Y + ui::scale(9),
                       (REAL)ui::scale(110), (REAL)ui::scale(22)),
                 ui::fontBody(), rc, 2);

        // 副行：地址 · 传输/格式
        std::string sub = s.host;
if (!s.name.empty() && s.level != ahub::BW_FULL) {
            sub += std::string(" · ") + ahub::bwLabel(s.level) + " "
                 + std::to_string(ahub::bwKbps(s.level)) + " kbps";
        }
        ui::text(gr, ui::toWide(sub),
                 RectF(innerL, hit.card.Y + ui::scale(34), innerW, (REAL)ui::scale(18)),
                 ui::fontCaption(), ui::color::textTertiary(), 0);

        // 实时电平条
        RectF bar(innerL, hit.card.Y + ui::scale(58), innerW, (REAL)ui::scale(6));
        ui::levelBar(gr, bar, s.levelPercent / 100.0f, s.muted || !s.connected);

        // 音量滑块
        bool sHover = (a.hoverSlider == (int)i) || (a.dragSlider == (int)i);
        ui::slider(gr, hit.slider, s.gain / 2.0f, sHover, a.dragSlider == (int)i);

        // 音量数值
        swprintf(rbuf, 96, L"%d%%", (int)(s.gain * 100));
        ui::text(gr, rbuf,
                 RectF(hit.slider.GetRight() + ui::scale(8),
                       hit.slider.Y, (REAL)ui::scale(52), hit.slider.Height),
                 ui::fontCaption(), ui::color::textSecond(), 0);

        // 静音按钮
        ui::outlineButton(gr, hit.mute, s.muted ? L"已静音" : L"静音",
                          ui::fontCaption(), a.hoverMute == (int)i, s.muted);
    }
}

void paintStatus(App& a, Graphics& gr, int W, int H) {
    const int y = H - ui::scale(STATUS_H);
    const int pad = ui::scale(PAD);

    Pen sep(ui::color::strokeSoft(), 1.0f);
    gr.DrawLine(&sep, (REAL)pad, (REAL)y, (REAL)(W - pad), (REAL)y);

    std::string left = "局域网 " + a.opt.localIp + " · 端口 " + std::to_string(a.opt.port)
                     + " · 接收设备 " + std::to_string(a.player->clipboardPeerCount());
    ui::text(gr, ui::toWide(left),
             RectF((REAL)pad, (REAL)y, (REAL)(W - pad * 2), (REAL)ui::scale(STATUS_H)),
             ui::fontCaption(), ui::color::textTertiary(), 0);
}

void paintToast(App& a, Graphics& gr, int W, int H) {
    if (a.actionNotice.empty() || a.noticeUntil <= GetTickCount()) return;
    const int pad = ui::scale(PAD);
    RectF r((REAL)(pad + ui::scale(30)),
            (REAL)(H - ui::scale(STATUS_H + 58)),
            (REAL)(W - pad * 2 - ui::scale(60)), (REAL)ui::scale(42));
    Color stroke = ui::color::accentDim();
    ui::roundedRect(gr, r, (REAL)ui::scale(11), ui::color::layerHover(), &stroke, 1.0f);
    ui::text(gr, a.actionNotice, r, ui::fontBody(), ui::color::text(), 1);
}

// ==================================================================
// 绘制入口（双缓冲）
// ==================================================================
void render(App& a) {
    RECT rc;
    GetClientRect(a.hwnd, &rc);
    const int W = rc.right - rc.left;
    const int H = rc.bottom - rc.top;
    if (W <= 0 || H <= 0) return;

    if (!a.memDC || a.memW != W || a.memH != H) {
        if (a.memBmp) DeleteObject(a.memBmp);
        if (a.memDC)  DeleteDC(a.memDC);
        HDC screen = GetDC(a.hwnd);
        a.memDC  = CreateCompatibleDC(screen);
        a.memBmp = CreateCompatibleBitmap(screen, W, H);
        SelectObject(a.memDC, a.memBmp);
        ReleaseDC(a.hwnd, screen);
        a.memW = W;
        a.memH = H;
    }

    layout(a, W, H);

    {
        Graphics gr(a.memDC);
        gr.SetSmoothingMode(SmoothingModeAntiAlias);
        gr.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        gr.SetPixelOffsetMode(PixelOffsetModeHalf);

        SolidBrush bg(ui::color::windowBg());
        gr.FillRectangle(&bg, 0, 0, W, H);

        paintTitleBar(a, gr, W);
        paintMode(a, gr, W);
        if (a.senderMode) {
            paintSender(a, gr, W);
            paintSenderPeers(a, gr, W, H);
        } else {
            paintReceiverService(a, gr, W);
            paintMaster(a, gr, W);
            paintCards(a, gr, W, H);
        }
        paintStatus(a, gr, W, H);
        paintToast(a, gr, W, H);
    }

    // 一次性贴到窗口，避免闪烁
    HDC dc = GetDC(a.hwnd);
    BitBlt(dc, 0, 0, W, H, a.memDC, 0, 0, SRCCOPY);
    ReleaseDC(a.hwnd, dc);
}

// ==================================================================
// 鼠标交互
// ==================================================================
bool inRect(const RectF& r, int x, int y) {
    return x >= r.X && x <= r.GetRight() && y >= r.Y && y <= r.GetBottom();
}

void showToast(App& a, const std::wstring& message) {
    a.actionNotice = message;
    a.noticeUntil = GetTickCount() + 2800;
    if (IsWindowVisible(a.hwnd)) InvalidateRect(a.hwnd, nullptr, FALSE);
    else trayNotice(a, message);
}

void updateHover(App& a, int x, int y, int W) {
    a.hoverClose = inRect(titleBtn(W, true), x, y);
    a.hoverMin   = inRect(titleBtn(W, false), x, y);
    a.hoverMaster = !a.senderMode && inRect(masterRect(W), x, y);
    a.hoverClipboardSend = a.senderMode && inRect(clipboardSendRect(W), x, y);
    a.hoverReceiverClipboard = !a.senderMode && inRect(receiverClipboardRect(W), x, y);
    a.hoverReceiverScan = !a.senderMode && inRect(receiverScanRect(W), x, y);
    a.hoverSenderScan = a.senderMode && inRect(senderScanRect(W), x, y);
    a.hoverReceiverService = !a.senderMode && inRect(receiverServiceRect(W), x, y);
    a.hoverSenderService = a.senderMode && inRect(senderServiceRect(W), x, y);
    a.hoverSlider = -1;
    a.hoverMute   = -1;
    a.hoverPeer   = -1;
    if (a.senderMode) {
        for (size_t i = 0; i < a.peerHits.size(); ++i)
            if (inRect(a.peerHits[i].toggle, x, y)) a.hoverPeer = (int)i;
    }
    for (size_t i = 0; i < a.hits.size(); i++) {
        if (!inRect(a.hits[i].card, x, y)) continue;
        if (inRect(a.hits[i].slider, x, y)) a.hoverSlider = (int)i;
        if (inRect(a.hits[i].mute, x, y))   a.hoverMute   = (int)i;
    }
}

void onMouseMove(App& a, int x, int y) {
    RECT rc;
    GetClientRect(a.hwnd, &rc);
    const int W = rc.right;

    if (!a.tracking) {
        TRACKMOUSEEVENT tme;
        memset(&tme, 0, sizeof(tme));
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = a.hwnd;
        TrackMouseEvent(&tme);
        a.tracking = true;
    }

    if (a.dragMaster) {
        RectF r = masterRect(W);
        float v = (x - r.X) / r.Width;
        v = std::max(0.0f, std::min(1.0f, v));
        a.player->setMasterGain(v);
        InvalidateRect(a.hwnd, nullptr, FALSE);
        return;
    }
    if (a.dragSlider >= 0 && a.dragSlider < (int)a.hits.size()) {
        RectF r = a.hits[a.dragSlider].slider;
        float v = (x - r.X) / r.Width;
        v = std::max(0.0f, std::min(1.0f, v));
        // 滑块量程 0~200%（0~2 倍增益）
        a.player->setSourceGain(a.hits[a.dragSlider].host, v * 2.0f);
        InvalidateRect(a.hwnd, nullptr, FALSE);
        return;
    }

    updateHover(a, x, y, W);
    InvalidateRect(a.hwnd, nullptr, FALSE);
}

void onLButtonDown(App& a, int x, int y) {
    RECT rc;
    GetClientRect(a.hwnd, &rc);
    const int W = rc.right;

    if (inRect(titleBtn(W, true), x, y)) {
        DestroyWindow(a.hwnd);
        return;
    }
    if (inRect(titleBtn(W, false), x, y)) {
        SendMessageW(a.hwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);
        return;
    }
    if (inRect(modeRect(W, false), x, y) || inRect(modeRect(W, true), x, y)) {
        a.senderMode = inRect(modeRect(W, true), x, y);
        a.dragMaster = false;
        a.dragSlider = -1;
        InvalidateRect(a.hwnd, nullptr, FALSE);
        return;
    }
    if (!a.senderMode && inRect(receiverServiceRect(W), x, y)) {
        int state = a.opt.receiverState ? a.opt.receiverState() : 0;
        if (state == 1 || state == 3) return;
        bool start = state == 0;
        std::string err;
        if (a.opt.toggleReceiver && a.opt.toggleReceiver(start, err)) {
            a.lastReceiverState = start ? 1 : 3;
            showToast(a, start ? L"正在启动接收" : L"正在停止接收");
        } else
            showToast(a, L"接收启动失败：" + ui::toWide(err));
        return;
    }
    if (!a.senderMode && inRect(receiverScanRect(W), x, y)) {
        if (a.opt.requestReceiverScan && a.opt.requestReceiverScan())
            showToast(a, L"正在扫描局域网设备，约 5 秒");
        else showToast(a, L"请先开始接收，或等待当前扫描结束");
        return;
    }
    if (a.senderMode && inRect(senderServiceRect(W), x, y)) {
        int state = a.opt.senderState ? a.opt.senderState() : 0;
        if (state == 1 || state == 3) return;
        bool start = state == 0;
        std::string err;
        if (a.opt.toggleSender && a.opt.toggleSender(start, err)) {
            a.lastSenderState = start ? 1 : 3;
            showToast(a, start ? L"正在启动发送" : L"正在停止发送");
        } else
            showToast(a, L"发送切换失败：" + ui::toWide(err));
        return;
    }
    if (a.senderMode && inRect(senderScanRect(W), x, y)) {
        if (a.opt.requestSenderScan && a.opt.requestSenderScan())
            showToast(a, L"正在扫描局域网设备，约 5 秒");
        else showToast(a, L"请先开始发送，或等待当前扫描结束");
        return;
    }
    if ((a.senderMode && inRect(clipboardSendRect(W), x, y)) ||
            (!a.senderMode && inRect(receiverClipboardRect(W), x, y))) {
        int sent = a.player->sendClipboard();
        showToast(a, sent > 0 ? L"剪贴板已发送给 " + std::to_wstring(sent) + L" 台设备"
                              : L"尚无已连接设备");
        return;
    }
    if (a.senderMode) {
        std::vector<ahub::ClipboardServer::PeerStat> peers = a.player->senderPeers();
        for (const auto& hit : a.peerHits) {
            if (!inRect(hit.toggle, x, y)) continue;
            auto it = std::find_if(peers.begin(), peers.end(), [&](const auto& p) {
                return p.address == hit.address;
            });
            if (it != peers.end() && a.player->setSenderPeerEnabled(hit.address, !it->audioEnabled))
                showToast(a, it->audioEnabled ? L"已暂停向该设备发送声音" : L"已恢复向该设备发送声音");
            return;
        }
    }
    if (!a.senderMode && inRect(masterRect(W), x, y)) {
        a.dragMaster = true;
        SetCapture(a.hwnd);
        float v = (x - masterRect(W).X) / masterRect(W).Width;
        a.player->setMasterGain(std::max(0.0f, std::min(1.0f, v)));
        InvalidateRect(a.hwnd, nullptr, FALSE);
        return;
    }

    for (size_t i = 0; i < a.hits.size(); i++) {
        if (inRect(a.hits[i].mute, x, y)) {
            std::vector<ahub::SourceStat> st = a.player->stats();
            if (i < st.size()) a.player->setSourceMute(a.hits[i].host, !st[i].muted);
            InvalidateRect(a.hwnd, nullptr, FALSE);
            return;
        }
        if (inRect(a.hits[i].slider, x, y)) {
            a.dragSlider = (int)i;
            SetCapture(a.hwnd);
            RectF r = a.hits[i].slider;
            float v = (x - r.X) / r.Width;
            a.player->setSourceGain(a.hits[i].host, std::max(0.0f, std::min(1.0f, v)) * 2.0f);
            InvalidateRect(a.hwnd, nullptr, FALSE);
            return;
        }
    }
}

void onLButtonUp(App& a) {
    if (a.dragMaster || a.dragSlider >= 0) {
        a.dragMaster = false;
        a.dragSlider = -1;
        ReleaseCapture();
        InvalidateRect(a.hwnd, nullptr, FALSE);
    }
}

void onMouseWheel(App& a, int delta) {
    if (a.maxScroll <= 0) return;
    a.scrollY -= (float)delta / 120.0f * ui::scale(60);
    if (a.scrollY < 0) a.scrollY = 0;
    if (a.scrollY > a.maxScroll) a.scrollY = a.maxScroll;
    InvalidateRect(a.hwnd, nullptr, FALSE);
}

// ==================================================================
// 窗口过程
// ==================================================================
LRESULT hitTest(App& a, int sx, int sy) {
    RECT wr;
    GetWindowRect(a.hwnd, &wr);
    int x = sx - wr.left;
    int y = sy - wr.top;
    const int W = wr.right - wr.left;
    const int H = wr.bottom - wr.top;
    const int m = ui::scale(RESIZE_M);

    const bool left   = x < m;
    const bool right  = x > W - m;
    const bool top    = y < m;
    const bool bottom = y > H - m;

    if (bottom && left)  return HTBOTTOMLEFT;
    if (bottom && right) return HTBOTTOMRIGHT;
    if (top && left)     return HTTOPLEFT;
    if (top && right)    return HTTOPRIGHT;
    if (left)   return HTLEFT;
    if (right)  return HTRIGHT;
    if (top)    return HTTOP;
    if (bottom) return HTBOTTOM;

    // 标题栏区域可拖动窗口；其上的按钮除外
    if (y < ui::scale(TITLE_H)) {
        if (inRect(titleBtn(W, true), x, y))  return HTCLIENT;
        if (inRect(titleBtn(W, false), x, y)) return HTCLIENT;
        return HTCAPTION;
    }
    return HTCLIENT;
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App& a = g;

    // Explorer recreates the notification area after a restart.
    if (taskbarCreated && msg == taskbarCreated) {
        a.trayAdded = false;
        addTrayIcon(a);
        return 0;
    }

    switch (msg) {
    case WM_CREATE: {
        a.hwnd = hwnd;
        // Win11 圆角 + 深色标题栏 + 深色边框
        BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        int corner = DWMWCP_ROUND;
        DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
        // 边框颜色与自绘背景一致，避免出现一圈亮边
        COLORREF bc = RGB(60, 60, 60);
        DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &bc, sizeof(bc));
        SetTimer(hwnd, 1, 250, nullptr); // 前台 4fps，后台降低刷新频率
        return 0;
    }

    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_MINIMIZE && (a.trayAdded || addTrayIcon(a))) {
            minimizeToTray(a);
            return 0;
        }
        break;

    case WM_SIZE:
        KillTimer(hwnd, 1);
        SetTimer(hwnd, 1, wp == SIZE_MINIMIZED ? 1000 : 250, nullptr);
        if (wp == SIZE_MINIMIZED && a.trayAdded)
            PostMessageW(hwnd, TRAY_MINIMIZE, 0, 0);
        return 0;

    case TRAY_MINIMIZE:
        if (IsIconic(hwnd) && a.trayAdded) minimizeToTray(a);
        return 0;

    case TRAY_MESSAGE:
        if (lp == WM_LBUTTONDBLCLK) restoreFromTray(a);
        else if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) showTrayMenu(a);
        return 0;

    case WM_TIMER:
        if (a.player && a.player->clipboardReceiveSeq() != a.lastClipboardSeq) {
            a.lastClipboardSeq = a.player->clipboardReceiveSeq();
            showToast(a, L"已收到剪贴板内容，已复制到电脑剪贴板");
        }
        if (a.opt.receiverState) {
            int state = a.opt.receiverState();
            if (state != a.lastReceiverState) {
                if (a.lastReceiverState == 1 && state == 2) showToast(a, L"已开始接收");
                else if (a.lastReceiverState == 1 && state == 0)
                    showToast(a, L"接收启动失败：" + ui::toWide(a.opt.receiverError()));
                else if (a.lastReceiverState == 3 && state == 0) showToast(a, L"已停止接收");
                a.lastReceiverState = state;
            }
        }
        if (a.opt.senderState) {
            int state = a.opt.senderState();
            if (state != a.lastSenderState) {
                if (a.lastSenderState == 1 && state == 2) {
                    std::string error = a.opt.senderError();
                    showToast(a, error.empty() ? L"已开始发送，正在连接手机" : ui::toWide(error));
                } else if (a.lastSenderState == 1 && state == 0)
                    showToast(a, L"发送启动失败：" + ui::toWide(a.opt.senderError()));
                else if (a.lastSenderState == 3 && state == 0) showToast(a, L"已停止发送");
                a.lastSenderState = state;
            }
        }
        if (IsWindowVisible(hwnd)) InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_ERASEBKGND:
        return 1;   // 全部自绘，禁止系统擦背景（防闪烁）

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        render(a);
        EndPaint(hwnd, &ps);
        (void)dc;
        return 0;
    }

    case WM_NCHITTEST:
        return hitTest(a, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));

    case WM_MOUSEMOVE:
        onMouseMove(a, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;

    case WM_MOUSELEAVE:
        a.tracking = false;
        a.hoverClose = a.hoverMin = a.hoverMaster = false;
        a.hoverClipboardSend = false;
        a.hoverReceiverClipboard = false;
        a.hoverReceiverScan = false;
        a.hoverSenderScan = false;
        a.hoverReceiverService = false;
        a.hoverSenderService = false;
        a.hoverSlider = a.hoverMute = a.hoverPeer = -1;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_LBUTTONDOWN:
        onLButtonDown(a, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;

    case WM_LBUTTONUP:
        onLButtonUp(a);
        return 0;

    case WM_MOUSEWHEEL:
        onMouseWheel(a, GET_WHEEL_DELTA_WPARAM(wp));
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lp;
        mmi->ptMinTrackSize.x = ui::scale(420);
        mmi->ptMinTrackSize.y = ui::scale(360);
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hwnd, 1);
        removeTrayIcon(a);
        PostQuitMessage(a.exitCode);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

} // namespace

// ==================================================================
int run(HINSTANCE hInst, ahub::Player* player, const Options& opt) {
    g.player = player;
    g.opt    = opt;

    // DPI 感知由 app.manifest 里的 dpiAwareness=PerMonitorV2 声明，
    // 不再运行时调用 SetProcessDPIAware() —— 清单方式更规范，
    // 多显示器不同缩放时也正确。
    {
        HDC dc = GetDC(nullptr);
        int dpi = GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(nullptr, dc);
        ui::setDpi(dpi);
    }

    GdiplusStartupInput gpIn;
    ULONG_PTR gpTok = 0;
    if (GdiplusStartup(&gpTok, &gpIn, nullptr) != Ok) {
        MessageBoxW(nullptr, L"GDI+ 初始化失败，无法创建界面", L"AudioHub", MB_ICONERROR);
        return 1;
    }

    taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = wndProc;
    wc.hInstance     = hInst;
    wc.hIcon         = LoadIconW(hInst, MAKEINTRESOURCEW(101));
    wc.hIconSm       = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(101), IMAGE_ICON,
                                        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                        LR_DEFAULTCOLOR);
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"AudioHubFluent";
    if (!RegisterClassExW(&wc)) {
        GdiplusShutdown(gpTok);
        return 1;
    }

    // 居中显示
    int W = ui::scale(520), H = ui::scale(660);
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    int x = (sw - W) / 2, y = (sh - H) / 2;

    HWND hwnd = CreateWindowExW(
            0, wc.lpszClassName, L"AudioHub",
            // WS_THICKFRAME 提供可拖拽改变大小的边框（配合自定义 NCHITTEST）；
            // 不用 WS_OVERLAPPEDWINDOW 是为了去掉系统标题栏，改自绘 Win11 样式
            WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX,
            x, y, W, H, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) {
        GdiplusShutdown(gpTok);
        return 1;
    }
    g.hwnd = hwnd;
    addTrayIcon(g);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (g.memBmp) DeleteObject(g.memBmp);
    if (g.memDC)  DeleteDC(g.memDC);
    ui::releaseFonts();
    GdiplusShutdown(gpTok);
    return (int)msg.wParam;
}

} // namespace uiwin


