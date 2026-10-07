package com.audiohub.probe;

import android.Manifest;
import android.content.Intent;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.pm.PackageManager;
import android.media.projection.MediaProjectionManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import androidx.appcompat.app.AppCompatActivity;
import androidx.core.graphics.Insets;
import androidx.core.view.ViewCompat;
import androidx.core.view.WindowCompat;
import androidx.core.view.WindowInsetsCompat;

import com.google.android.material.appbar.MaterialToolbar;
import com.google.android.material.button.MaterialButton;
import com.google.android.material.button.MaterialButtonToggleGroup;
import com.google.android.material.card.MaterialCardView;
import com.google.android.material.materialswitch.MaterialSwitch;
import com.google.android.material.progressindicator.LinearProgressIndicator;
import com.google.android.material.slider.Slider;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * AudioHub 主界面 —— Material Design 3 双模式
 *
 *   发送：MediaProjection 内录本机声音 → 本机增益 → TCP 广播，可逐个停用接收端
 *   接收：扫描局域网 → 可连接设备列表（可勾选）→ 每路独立抖动缓冲 → 混音 → AudioTrack 输出
 */
public class MainActivity extends AppCompatActivity {

    private static final int REQ_PERM       = 1001;
    private static final int REQ_PROJECTION = 1002;

    private MaterialToolbar toolbar;
    private MaterialButtonToggleGroup toggleMode;
    private View panelSource, panelPlayer;

    // ---------- 发送 ----------
    private TextView txtVerdict, txtStats, txtLog, txtNet, txtVolume;
    private LinearProgressIndicator barLevel;
    private MaterialButton btnStart, btnStop;
    private MaterialButton btnSendClipboard;
    private Slider sliderVolume;
    private LinearLayout receiverList;
    private TextView txtNoReceiver;

    // ---------- 接收 ----------
    private TextView txtPlayerState, txtMaster, txtPlayerLog;
    private MaterialButton btnPlayerStart, btnPlayerStop;
    private MaterialButton btnPlayerRefresh, btnPlayerSendClipboard;
    private LinearLayout playerSources;
    private MaterialCardView cardNoSource;
    private Slider sliderMaster;

    private final Map<String, SourceCard>  cards    = new LinkedHashMap<>();
    private final Map<String, ReceiverRow> receivers = new LinkedHashMap<>();

    private final Handler handler = new Handler(Looper.getMainLooper());

    /** 接收端里「一路音源」的界面引用 */
    private static final class SourceCard {
        View root, dot;
        TextView host, connState, gainText, stats;
        Slider slider;
        MaterialButton mute;
        MaterialSwitch enable;
        LinearProgressIndicator meter;
        boolean syncing;
    }

    /** 发送端里「一个接收端」的界面引用 */
    private static final class ReceiverRow {
        View root, dotView, approveBox;
        TextView name, addr, latency;
        MaterialSwitch send;
        MaterialButton btnAllow, btnDeny;
        boolean syncing;
    }

    // ==================================================================
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        WindowCompat.setDecorFitsSystemWindows(getWindow(), false);
        SystemBars.apply(this);
        setContentView(R.layout.activity_main);

        toolbar           = findViewById(R.id.toolbar);
        toggleMode        = findViewById(R.id.toggleMode);
        panelSource       = findViewById(R.id.panelSource);
        panelPlayer       = findViewById(R.id.panelPlayer);

        txtVerdict        = findViewById(R.id.txtVerdict);
        barLevel          = findViewById(R.id.barLevel);
        txtStats          = findViewById(R.id.txtStats);
        txtLog            = findViewById(R.id.txtLog);
        txtNet            = findViewById(R.id.txtNet);
        txtVolume         = findViewById(R.id.txtVolume);
        btnStart          = findViewById(R.id.btnStart);
        btnStop           = findViewById(R.id.btnStop);
        btnSendClipboard  = findViewById(R.id.btnSendClipboard);
        sliderVolume      = findViewById(R.id.sliderVolume);
        receiverList      = findViewById(R.id.receiverList);
        txtNoReceiver     = findViewById(R.id.txtNoReceiver);

        txtPlayerState  = findViewById(R.id.txtPlayerState);
        txtMaster       = findViewById(R.id.txtMaster);
        txtPlayerLog    = findViewById(R.id.txtPlayerLog);
        btnPlayerStart  = findViewById(R.id.btnPlayerStart);
        btnPlayerStop   = findViewById(R.id.btnPlayerStop);
        btnPlayerRefresh = findViewById(R.id.btnPlayerRefresh);
        btnPlayerSendClipboard = findViewById(R.id.btnPlayerSendClipboard);
        playerSources   = findViewById(R.id.playerSources);
        cardNoSource    = findViewById(R.id.cardNoSource);
        sliderMaster    = findViewById(R.id.sliderMaster);

        applyWindowInsets();
        setupToolbar();
        setupModeToggle(savedInstanceState);
        setupSourcePanel();
        setupPlayerPanel();

        requestNeededPermissions();
    }

    // ==================================================================
    /**
     * Android 15+ 强制 edge-to-edge，必须自己避让状态栏。
     *
     * ⚠️ 关键点：工具栏高度是固定的 ?attr/actionBarSize，如果只加 paddingTop，
     *    标题会被挤到剩余的那几 dp 里直接看不见。
     *    正确做法是把工具栏【总高度】增加状态栏高度，再在内部让出顶部内边距。
     */
    private void applyWindowInsets() {
        View content = findViewById(android.R.id.content);
        View scroll  = findViewById(R.id.scrollRoot);

        int base = toolbar.getLayoutParams().height;
        if (base <= 0) {
            base = (int) (56 * getResources().getDisplayMetrics().density);
        }
        final int baseHeight = base;

        ViewCompat.setOnApplyWindowInsetsListener(content, (v, insets) -> {
            Insets sb = insets.getInsets(WindowInsetsCompat.Type.systemBars());

            ViewGroup.LayoutParams lp = toolbar.getLayoutParams();
            int want = baseHeight + sb.top;
            if (lp.height != want) {
                lp.height = want;
                toolbar.setLayoutParams(lp);
            }
            toolbar.setPadding(toolbar.getPaddingLeft(), sb.top,
                    toolbar.getPaddingRight(), toolbar.getPaddingBottom());

            scroll.setPadding(scroll.getPaddingLeft(), scroll.getPaddingTop(),
                    scroll.getPaddingRight(), sb.bottom);
            return insets;
        });
        ViewCompat.requestApplyInsets(content);
    }

    private void setupToolbar() {
        toolbar.inflateMenu(R.menu.main_menu);
        toolbar.setOnMenuItemClickListener(item -> {
            if (item.getItemId() == R.id.action_settings) {
                startActivity(new Intent(this, SettingsActivity.class));
                return true;
            }
            return false;
        });
    }

    // ==================================================================
    private void setupModeToggle(Bundle savedInstanceState) {
        boolean showPlayer = savedInstanceState == null
                || savedInstanceState.getBoolean("playerMode", true);
        toggleMode.check(showPlayer ? R.id.btnModePlayer : R.id.btnModeSource);
        panelSource.setVisibility(showPlayer ? View.GONE : View.VISIBLE);
        panelPlayer.setVisibility(showPlayer ? View.VISIBLE : View.GONE);

        toggleMode.addOnButtonCheckedListener((group, checkedId, isChecked) -> {
            if (!isChecked) return;
            boolean playerMode = (checkedId == R.id.btnModePlayer);
            panelPlayer.setVisibility(playerMode ? View.VISIBLE : View.GONE);
            panelSource.setVisibility(playerMode ? View.GONE : View.VISIBLE);
            PlayerJitterBuffer.setTargetMs(Settings.jitterTargetMs(this));
            if (playerMode) refreshPlayerUi();
        });
    }

    // ==================================================================
    // 发送
    // ==================================================================
    private void setupSourcePanel() {
        sliderVolume.setValue(CaptureState.inputGain * 100f);
        txtVolume.setText((int) (CaptureState.inputGain * 100) + "%");
        sliderVolume.addOnChangeListener((slider, value, fromUser) -> {
            CaptureState.inputGain = value / 100f;
            txtVolume.setText((int) value + "%");
        });
        findViewById(R.id.btnResetVolume).setOnClickListener(v -> sliderVolume.setValue(100f));

        btnStart.setOnClickListener(v -> onStartCaptureClicked());
        btnStop.setOnClickListener(v -> onStopClicked());
        btnSendClipboard.setOnClickListener(v -> sendClipboardFromSource());
        startClipboardNetwork();
    }

    @Override
    protected void onSaveInstanceState(Bundle outState) {
        outState.putBoolean("playerMode", toggleMode.getCheckedButtonId() == R.id.btnModePlayer);
        super.onSaveInstanceState(outState);
    }

    private void startClipboardNetwork() {
        AudioServer srv = CaptureService.activeServer();
        if (srv != null && srv.isRunning()) return;
        Intent svc = new Intent(this, CaptureService.class);
        svc.setAction(CaptureService.ACTION_CLIPBOARD_START);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) startForegroundService(svc);
        else startService(svc);
    }

    private String readClipboardText() {
        ClipboardManager cm = (ClipboardManager) getSystemService(CLIPBOARD_SERVICE);
        if (cm == null || !cm.hasPrimaryClip()) return "";
        ClipData d = cm.getPrimaryClip();
        if (d == null || d.getItemCount() == 0 || d.getItemAt(0) == null) return "";
        CharSequence t = d.getItemAt(0).coerceToText(this);
        return t == null ? "" : t.toString();
    }

    private void sendClipboardFromSource() {
        String text = readClipboardText();
        AudioServer srv = CaptureService.activeServer();
        if (srv == null || !srv.isRunning()) {
            startClipboardNetwork();
            Toast.makeText(this, UiText.tr(this, "已启动剪贴板网络服务，请刷新接收端后再次发送", "Clipboard service started. Refresh the receiver and send again."), Toast.LENGTH_LONG).show();
            return;
        }
        int n = CaptureService.sendClipboard(text);
        Toast.makeText(this, n > 0 ? UiText.tr(this, "剪贴板已发送", "Clipboard sent") : UiText.tr(this, "暂无已连接的接收端", "No connected receivers"), Toast.LENGTH_SHORT).show();
    }

    private void requestNeededPermissions() {
        List<String> need = new ArrayList<>();
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            need.add(Manifest.permission.RECORD_AUDIO);
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU
                && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
            need.add(Manifest.permission.POST_NOTIFICATIONS);
        }
        if (!need.isEmpty()) requestPermissions(need.toArray(new String[0]), REQ_PERM);
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == REQ_PERM) {
            for (int i = 0; i < permissions.length; i++) {
                if (Manifest.permission.RECORD_AUDIO.equals(permissions[i])
                        && grantResults[i] != PackageManager.PERMISSION_GRANTED) {
                    Toast.makeText(this, UiText.tr(this, "没有录音权限，无法发送", "Microphone permission is required to send audio"), Toast.LENGTH_LONG).show();
                }
            }
        }
    }

    private void onStartCaptureClicked() {
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            Toast.makeText(this, UiText.tr(this, "请先授予录音权限", "Grant microphone permission first"), Toast.LENGTH_LONG).show();
            requestNeededPermissions();
            return;
        }
        MediaProjectionManager mpm =
                (MediaProjectionManager) getSystemService(MEDIA_PROJECTION_SERVICE);
        if (mpm == null) {
            Toast.makeText(this, UiText.tr(this, "系统不支持 MediaProjection", "Screen audio capture is unavailable"), Toast.LENGTH_LONG).show();
            return;
        }
        startActivityForResult(mpm.createScreenCaptureIntent(), REQ_PROJECTION);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQ_PROJECTION) return;
        if (resultCode != RESULT_OK || data == null) {
            Toast.makeText(this, UiText.tr(this, "你拒绝了录屏授权，无法发送", "Capture permission denied; cannot send audio"), Toast.LENGTH_LONG).show();
            CaptureState.log("[授权] 用户拒绝");
            return;
        }
        CaptureState.log("[授权] 用户同意，启动发送服务");
        Intent svc = new Intent(this, CaptureService.class);
        svc.setAction(CaptureService.ACTION_START);
        svc.putExtra(CaptureService.EXTRA_RESULT_CODE, resultCode);
        svc.putExtra(CaptureService.EXTRA_RESULT_DATA, data);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) startForegroundService(svc);
        else startService(svc);
    }

    private void onStopClicked() {
        Intent svc = new Intent(this, CaptureService.class);
        svc.setAction(CaptureService.ACTION_STOP);
        try { startService(svc); } catch (Throwable t) { CaptureState.log("[停止] 指令发送失败: " + t); }
    }

    /** 刷新"可连接的接收端"列表（已连接的 + 广播发现的） */
    private void refreshReceiverList() {
        AudioServer srv = CaptureService.activeServer();
        List<AudioServer.Client> list = (srv == null) ? new ArrayList<>() : srv.clientList();

        // 已连接的接收端：addr -> Client
        java.util.Map<String, AudioServer.Client> connected = new java.util.HashMap<>();
        for (AudioServer.Client c : list) connected.put(c.addr, c);

        // 广播发现但尚未连接的接收端。
        // 连接方向是"接收端主动连发送端"，所以发送端无法主动连过去 ——
        // 这些条目只用于【告知用户对方在线】，不能从这边发起连接。
        List<String[]> available = new ArrayList<>();
        AnnounceListener an = CaptureService.activeAnnounce();
        if (an != null) {
            for (String[] a : an.available()) {
                if (!connected.containsKey(a[0])) available.add(a);
            }
        }

        int total = connected.size() + available.size();
        txtNoReceiver.setVisibility(total == 0 ? View.VISIBLE : View.GONE);

        // 移除已经消失的行（早期实现只加不删，会留下幽灵设备）
        java.util.Set<String> aliveAddrs = new java.util.HashSet<>(connected.keySet());
        for (String[] a : available) aliveAddrs.add(a[0]);
        java.util.Iterator<java.util.Map.Entry<String, ReceiverRow>> it = receivers.entrySet().iterator();
        while (it.hasNext()) {
            java.util.Map.Entry<String, ReceiverRow> e = it.next();
            if (!aliveAddrs.contains(e.getKey())) {
                receiverList.removeView(e.getValue().root);
                it.remove();
            }
        }

        // 已连接
        for (AudioServer.Client c : list) {
            ReceiverRow row = ensureRow(c.addr);
            boolean pending = (c.auth == Protocol.AUTH_PENDING);
            row.name.setText(c.name);
            row.addr.setText(c.addr);
            row.dotView.setBackgroundResource(pending ? R.drawable.dot_off : R.drawable.dot_on);
            row.approveBox.setVisibility(pending ? View.VISIBLE : View.GONE);

            if (pending) {
                row.latency.setText(UiText.tr(this, "⚠️ 请求接收你的音频 —— 点【允许】后才会开始发送", "⚠️ Wants to receive your audio. Tap Allow to start."));
            } else if (c.latencyMs >= 0) {
                row.latency.setText(String.format(Locale.US, UiText.tr(this, "🟢 已连接 · 延迟约 %d ms", "🟢 Connected · about %d ms latency"), c.latencyMs));
            } else {
                row.latency.setText(UiText.tr(this, "🟢 已连接 · 延迟测量中…", "🟢 Connected · measuring latency…"));
            }

            row.send.setEnabled(!pending);
            row.syncing = true;
            row.send.setChecked(!pending && c.enabled);
            row.syncing = false;
        }

        // 已发现但未连接
        for (String[] a : available) {
            ReceiverRow row = ensureRow(a[0]);
            row.name.setText(a[1]);
            row.addr.setText(a[0]);
            row.dotView.setBackgroundResource(R.drawable.dot_off);
            row.approveBox.setVisibility(View.GONE);
            row.latency.setText(UiText.tr(this, "⚪ 在线，等待对方连接（连接由接收端发起）", "⚪ Online; waiting for receiver to connect"));
            row.send.setEnabled(false);   // 发送端无法主动发起连接
            row.syncing = true;
            row.send.setChecked(false);
            row.syncing = false;
        }
    }

    /**
     * 取得（必要时创建）某个地址对应的行，并把所有监听器绑定好。
     *
     * ⚠️ 这里踩过两个坑，改的时候别再退回去：
     *
     *   1. 行是【按地址缓存、只创建一次】的，但早期「已连接」和「已发现」
     *      两个分支各写了一份建行代码，且只有「已连接」分支绑定了按钮监听器。
     *      电脑会先 UDP 广播、后 TCP 连接，于是行先由「已发现」分支创建，
     *      【允许】按钮永远没有监听器 —— 点了完全没反应。
     *
     *   2. 监听器早期直接捕获创建时的 Client 对象；对方重连会产生新的 Client，
     *      旧对象已失效，导致点按钮操作的是不存在的连接。
     *      现在一律【按地址动态查找当前连接】。
     */
    private ReceiverRow ensureRow(final String addr) {
        ReceiverRow row = receivers.get(addr);
        if (row != null) return row;

        View v = LayoutInflater.from(this).inflate(R.layout.item_receiver, receiverList, false);
        final ReceiverRow nr = new ReceiverRow();
        nr.root = v;
        nr.dotView = v.findViewById(R.id.dot);
        nr.name = v.findViewById(R.id.txtName);
        nr.addr = v.findViewById(R.id.txtAddr);
        nr.latency = v.findViewById(R.id.txtLatency);
        nr.send = v.findViewById(R.id.switchSend);
        nr.approveBox = v.findViewById(R.id.approveBox);
        nr.btnAllow = v.findViewById(R.id.btnAllow);
        nr.btnDeny = v.findViewById(R.id.btnDeny);

        nr.send.setOnCheckedChangeListener((b, checked) -> {
            if (nr.syncing) return;
            AudioServer srv = CaptureService.activeServer();
            if (srv == null) return;
            for (AudioServer.Client c : srv.clientList()) {
                if (c.addr.equals(addr)) {
                    c.setStreamEnabled(checked);
                    CaptureState.log("[发送] " + (checked ? "恢复发送到 " : "暂停发送到 ") + c.name);
                    break;
                }
            }
        });

        // 【允许】：放行并记住这台设备，以后不再询问
        nr.btnAllow.setOnClickListener(b -> {
            String name = nr.name.getText().toString();
            // 只负责写入「已批准设备」记录；授权由服务端的检查线程放行，
            // 不依赖这里能否匹配到连接对象。
            Settings.approve(MainActivity.this, addr, name);
            AudioServer srv = CaptureService.activeServer();
            if (srv != null) srv.setClientAuth(addr, Protocol.AUTH_APPROVED);
            CaptureState.log("[安全] 已允许 " + name + " 接收音频（已记住）");
            refreshReceiverList();
        });

        // 【拒绝】：断开连接并确保不在已批准列表里
        nr.btnDeny.setOnClickListener(b -> {
            AudioServer srv = CaptureService.activeServer();
            String name = nr.name.getText().toString();
            if (srv != null) {
                srv.setClientAuth(addr, Protocol.AUTH_DENIED);
                srv.disconnect(addr);
            }
            Settings.revoke(MainActivity.this, addr, name);
            CaptureState.log("[安全] 已拒绝 " + name);
            refreshReceiverList();
        });

        receivers.put(addr, nr);
        receiverList.addView(v);
        return nr;
    }

    // ==================================================================
    // 接收
    // ==================================================================
    private void setupPlayerPanel() {
        PlayerEngine engine = PlayerService.engine(this);
        sliderMaster.setValue(engine.masterGain * 100f);
        txtMaster.setText((int) (engine.masterGain * 100) + "%");
        sliderMaster.addOnChangeListener((slider, value, fromUser) -> {
            engine.masterGain = value / 100f;
            txtMaster.setText((int) value + "%");
        });
        findViewById(R.id.btnResetMaster).setOnClickListener(v -> sliderMaster.setValue(100f));

        btnPlayerStart.setOnClickListener(v -> {
            startPlayerAndRefresh(false);
        });

        btnPlayerStop.setOnClickListener(v -> {
            Intent svc = new Intent(this, PlayerService.class);
            svc.setAction(PlayerService.ACTION_STOP);
            try { startService(svc); } catch (Throwable ignored) {}
            CaptureState.log("[接收] 用户点击停止");
        });
        btnPlayerRefresh.setOnClickListener(v -> startPlayerAndRefresh(true));
        btnPlayerSendClipboard.setOnClickListener(v -> sendClipboardFromPlayer());
    }

    private void startPlayerAndRefresh(boolean refresh) {
        PlayerJitterBuffer.setTargetMs(Settings.jitterTargetMs(this));
        PlayerEngine engine = PlayerService.engine(this);
        if (!engine.isRunning()) {
            Intent svc = new Intent(this, PlayerService.class);
            svc.setAction(PlayerService.ACTION_START);
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) startForegroundService(svc);
            else startService(svc);
            CaptureState.log("[接收] 已启动接收服务");
        }
        if (refresh) {
            Runnable[] request = new Runnable[1];
            int[] attempts = {0};
            request[0] = () -> {
                if (engine.isRunning()) engine.refreshNow();
                else if (attempts[0]++ < 30) handler.postDelayed(request[0], 100);
            };
            handler.post(request[0]);
            Toast.makeText(this, UiText.tr(this, "正在扫描附近设备，约 5 秒", "Scanning nearby devices for about 5 seconds"), Toast.LENGTH_SHORT).show();
        }
    }

    private void sendClipboardFromPlayer() {
        String text = readClipboardText();
        PlayerEngine engine = PlayerService.engine(this);
        if (!engine.isRunning()) {
            startPlayerAndRefresh(false);
            Toast.makeText(this, UiText.tr(this, "已启动接收服务，请连接设备后再次发送", "Receiver started. Connect a device and send again."), Toast.LENGTH_LONG).show();
            return;
        }
        int n = engine.sendClipboard(text);
        Toast.makeText(this, n > 0 ? UiText.tr(this, "剪贴板已发送", "Clipboard sent") : UiText.tr(this, "暂无已连接的发送端", "No connected senders"), Toast.LENGTH_SHORT).show();
    }

    private void refreshPlayerUi() {
        PlayerEngine e = PlayerService.engine(this);
        boolean running = e.isRunning();

        btnPlayerStart.setEnabled(!running);
        btnPlayerStop.setEnabled(running);

        if (running) {
            txtPlayerState.setText(String.format(Locale.US, UiText.tr(this, "接收中 · %d 路已连接", "Receiving · %d connected"), e.connectedCount()));
        } else {
            txtPlayerState.setText(R.string.state_idle);
        }

        List<PlayerSource> srcs = e.sources();
        cardNoSource.setVisibility(srcs.isEmpty() ? View.VISIBLE : View.GONE);

        java.util.Set<String> activeHosts = new java.util.HashSet<>();
        for (PlayerSource s : srcs) activeHosts.add(s.host);
        java.util.Iterator<Map.Entry<String, SourceCard>> cardIt = cards.entrySet().iterator();
        while (cardIt.hasNext()) {
            Map.Entry<String, SourceCard> entry = cardIt.next();
            if (!activeHosts.contains(entry.getKey())) {
                playerSources.removeView(entry.getValue().root);
                cardIt.remove();
            }
        }

        for (PlayerSource s : srcs) {
            if (!cards.containsKey(s.host)) cards.put(s.host, createCard(s));
        }
        for (Map.Entry<String, SourceCard> en : cards.entrySet()) {
            for (PlayerSource s : srcs) {
                if (s.host.equals(en.getKey())) { updateCard(en.getValue(), s); break; }
            }
        }

        String log = CaptureState.logText();
        String[] lines = log.split("\n");
        int from = Math.max(0, lines.length - 12);
        StringBuilder sb = new StringBuilder();
        for (int i = from; i < lines.length; i++) sb.append(lines[i]).append('\n');
        txtPlayerLog.setText(CaptureState.logEnabled
                ? sb.toString() : getString(R.string.log_disabled_hint));
    }

    private SourceCard createCard(PlayerSource s) {
        View v = LayoutInflater.from(this).inflate(R.layout.item_player_source, playerSources, false);
        SourceCard c = new SourceCard();
        c.root      = v;
        c.dot       = v.findViewById(R.id.dot);
        c.host      = v.findViewById(R.id.txtHost);
        c.connState = v.findViewById(R.id.txtConnState);
        c.gainText  = v.findViewById(R.id.txtGain);
        c.stats     = v.findViewById(R.id.txtSourceStats);
        c.slider    = v.findViewById(R.id.sliderGain);
        c.mute      = v.findViewById(R.id.btnMute);
        c.enable    = v.findViewById(R.id.switchEnable);
        c.meter     = v.findViewById(R.id.barSourceLevel);

        c.host.setText(s.displayName());
        c.slider.setValue(s.gain * 100f);
        c.slider.addOnChangeListener((slider, value, fromUser) -> {
            if (c.syncing) return;
            s.gain = value / 100f;
            c.gainText.setText((int) value + "%");
        });
        c.mute.setOnClickListener(b -> s.muted = !s.muted);
        v.findViewById(R.id.btnResetGain).setOnClickListener(b -> {
            s.gain = 1f;
            c.slider.setValue(100f);
            c.gainText.setText("100%");
        });
        c.enable.setOnCheckedChangeListener((b, checked) -> {
            if (c.syncing) return;
            PlayerService.engine(this).setSourceEnabled(s, checked);
        });

        playerSources.addView(v);
        return c;
    }

    private void updateCard(SourceCard c, PlayerSource s) {
        // 名字可能在后来的握手里才拿到
        String dn = s.displayName();
        if (!dn.contentEquals(c.host.getText())) c.host.setText(dn);

        c.dot.setBackgroundResource(
                (s.enabled && s.isConnected()) ? R.drawable.dot_on : R.drawable.dot_off);

        String state;
        if (!s.enabled) state = UiText.tr(this, "未连接", "Disconnected");
        else if (s.isConnected()) state = !s.isRemoteStreamEnabled() || !s.isActive()
                ? UiText.tr(this, "已连接，等待发送", "Connected; waiting for audio") : getString(R.string.source_ready);
        else state = UiText.status(this, s.status());

        // 延迟：连上并预热后才有意义
        String lat = "";
        if (s.enabled && s.isConnected() && s.isWarmedUp()) {
            lat = String.format(Locale.US, UiText.tr(this, " · 延迟约 %d ms", " · about %d ms latency"), s.latencyMs);
        }
        c.connState.setText(state + lat + " · " + s.host);

        c.meter.setProgress(s.levelPercent);
        c.mute.setText(s.muted ? R.string.btn_unmute : R.string.btn_mute);

        if (!c.slider.isPressed()) {
            c.syncing = true;
            c.slider.setValue(s.gain * 100f);
            c.enable.setChecked(s.enabled);
            c.syncing = false;
            c.gainText.setText((int) (s.gain * 100) + "%");
        }

        c.stats.setText(String.format(Locale.US,
                UiText.tr(this, "%s · 缓冲 %dms · 收到 %d · 丢失 %d · 欠载 %d · %.1f dBFS", "%s · buffer %d ms · received %d · lost %d · underruns %d · %.1f dBFS"),
                UiText.format(this, s.formatText()),
                s.pendingMs(),
                s.statRecv(), s.statLost(), s.statUnderruns(), s.levelDb));
    }

    // ==================================================================
    private final Runnable poller = new Runnable() {
        @Override
        public void run() {
            refreshSourceUi();
            refreshPlayerUi();
            handler.postDelayed(this, 500);
        }
    };

    private void refreshSourceUi() {
        boolean running = CaptureState.running;

        btnStart.setEnabled(!running);
        btnStop.setEnabled(running);

        barLevel.setProgress(CaptureState.levelPercent);

        String verdict;
        if (CaptureState.error != null) {
            verdict = UiText.tr(this, "❌ 出错了：", "❌ Error: ") + UiText.status(this, CaptureState.error);
        } else if (running && CaptureState.sawSignal) {
            verdict = UiText.tr(this, "✅ 正在发送，已收到声音", "✅ Sending audio");
        } else if (running && CaptureState.bytesTotal > 0) {
            verdict = UiText.tr(this, "⚠️ 有数据但全是静音\n系统或目标应用禁止被录制", "⚠️ Audio is silent\nThe system or source app may block capture");
        } else if (running) {
            verdict = UiText.tr(this, "⏳ 等待声音…", "⏳ Waiting for audio…");
        } else {
            verdict = getString(R.string.state_idle);
        }
        txtVerdict.setText(verdict);

        long since = CaptureState.connectedSinceMs;
        long secs = since > 0 ? (SystemClock.elapsedRealtime() - since) / 1000 : 0;
        txtStats.setText(String.format(Locale.US,
                UiText.tr(this, "连接时长 %d 秒\nRMS %.1f dBFS", "Connected for %d s\nRMS %.1f dBFS"), secs, CaptureState.rmsDb));

        txtLog.setText(CaptureState.logEnabled
                ? CaptureState.logText() : getString(R.string.log_disabled_hint));
        txtNet.setText(UiText.status(this, CaptureState.netInfo));

        refreshReceiverList();
    }

    @Override
    protected void onStart() {
        super.onStart();
        AppState.onActivityStart();
    }

    @Override
    protected void onStop() {
        super.onStop();
        AppState.onActivityStop();
    }

    @Override
    protected void onResume() {
        super.onResume();
        AppState.metersVisible = true;
        Settings.applyRuntime(this);   // 应用设置（日志开关、缓冲档位）
        handler.removeCallbacks(poller);
        handler.post(poller);
    }

    @Override
    protected void onPause() {
        AppState.metersVisible = false;
        handler.removeCallbacks(poller);
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        handler.removeCallbacks(poller);
    }
}
