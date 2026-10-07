package com.audiohub.probe;

import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.view.View;
import android.view.ViewGroup;
import android.widget.RadioGroup;
import android.widget.TextView;
import android.widget.Toast;

import androidx.appcompat.app.AppCompatActivity;
import androidx.appcompat.app.AppCompatDelegate;
import androidx.core.graphics.Insets;
import androidx.core.view.ViewCompat;
import androidx.core.view.WindowCompat;
import androidx.core.view.WindowInsetsCompat;

import com.google.android.material.appbar.MaterialToolbar;
import com.google.android.material.button.MaterialButton;
import com.google.android.material.button.MaterialButtonToggleGroup;
import com.google.android.material.materialswitch.MaterialSwitch;
import com.google.android.material.radiobutton.MaterialRadioButton;
import com.google.android.material.textfield.TextInputEditText;
import com.google.android.material.textfield.TextInputLayout;

/**
 * 设置页
 *
 * 命名说明：设置项按专业音频软件的习惯命名，并让每一项的说明如实反映其作用，
 * 而不是给一个听起来好但没有实际效果的选项。
 */
public class SettingsActivity extends AppCompatActivity {

    private TextInputEditText editName;
    private TextInputLayout    layoutName;
    private MaterialButtonToggleGroup toggleQuality;
    private TextView           txtQualityHint;
    private MaterialSwitch     switchExclusive, switchAutoConnect;
    private RadioGroup         radioBandwidth;

    /** 带宽档位对应的单选按钮 id，顺序与 Protocol.BW_* 一致 */
    private static final int[] BW_IDS = {
            R.id.rbBw0, R.id.rbBw1, R.id.rbBw2, R.id.rbBw3
    };


    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        WindowCompat.setDecorFitsSystemWindows(getWindow(), false);
        SystemBars.apply(this);
        setContentView(R.layout.activity_settings);

        MaterialToolbar toolbar = findViewById(R.id.settingsToolbar);
        View root        = findViewById(R.id.settingsRoot);
        View scroll      = findViewById(R.id.settingsScroll);

        // 状态栏 / 导航栏避让（Android 15+ 强制 edge-to-edge）
        // 注意：工具栏高度固定，必须把【总高度】加上状态栏高度，只加 paddingTop 会把标题挤没
        int base = toolbar.getLayoutParams().height;
        if (base <= 0) base = (int) (56 * getResources().getDisplayMetrics().density);
        final int baseHeight = base;

        ViewCompat.setOnApplyWindowInsetsListener(root, (v, insets) -> {
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
        ViewCompat.requestApplyInsets(root);

        toolbar.setNavigationOnClickListener(v -> finish());

        editName          = findViewById(R.id.editDeviceName);
        layoutName        = findViewById(R.id.layoutDeviceName);
        toggleQuality     = findViewById(R.id.toggleQuality);
        txtQualityHint    = findViewById(R.id.txtQualityHint);
        switchExclusive   = findViewById(R.id.switchExclusive);
        switchAutoConnect = findViewById(R.id.switchAutoConnect);

        // ---- 设备显示名称 ----
        editName.setText(Settings.deviceName(this));
        layoutName.setHelperText(getString(R.string.settings_device_name_default,
                Settings.defaultDeviceName()));

        // ---- 音频质量与延迟 ----
        int q = Settings.quality(this);
        toggleQuality.check(q == Settings.Q_LOW_LATENCY ? R.id.btnQLow
                : q == Settings.Q_STABLE ? R.id.btnQStable : R.id.btnQBalanced);
        updateQualityHint(q);
        toggleQuality.addOnButtonCheckedListener((group, checkedId, isChecked) -> {
            if (!isChecked) return;
            int nq = (checkedId == R.id.btnQLow) ? Settings.Q_LOW_LATENCY
                    : (checkedId == R.id.btnQStable) ? Settings.Q_STABLE : Settings.Q_BALANCED;
            Settings.setQuality(this, nq);
            PlayerJitterBuffer.setTargetMs(Settings.jitterTargetMs(this));
            AudioServer srv = CaptureService.activeServer();
            if (srv != null) srv.refreshQuality();
            updateQualityHint(nq);
        });

        // ---- 独占音频 ----
        switchExclusive.setChecked(Settings.exclusiveAudio(this));
        switchExclusive.setOnCheckedChangeListener((b, v) -> Settings.setExclusiveAudio(this, v));

        // ---- 自动连接 ----
        switchAutoConnect.setChecked(Settings.autoConnect(this));
        switchAutoConnect.setOnCheckedChangeListener((b, v) -> Settings.setAutoConnect(this, v));

        MaterialSwitch switchDarkMode = findViewById(R.id.switchDarkMode);
        switchDarkMode.setChecked(Settings.darkMode(this));
        switchDarkMode.setOnCheckedChangeListener((b, enabled) -> {
            Settings.setDarkMode(this, enabled);
            AppCompatDelegate.setDefaultNightMode(enabled
                    ? AppCompatDelegate.MODE_NIGHT_YES : AppCompatDelegate.MODE_NIGHT_NO);
        });

        // ---- 运行日志 ----
        MaterialSwitch switchLog = findViewById(R.id.switchLog);
        switchLog.setChecked(Settings.logEnabled(this));
        switchLog.setOnCheckedChangeListener((b, v) -> {
            Settings.setLogEnabled(this, v);
            Settings.applyRuntime(this);   // 实时生效，无需重启
        });

        // ---- 连接审批 ----
        MaterialSwitch switchApproval = findViewById(R.id.switchApproval);
        switchApproval.setChecked(Settings.approvalRequired(this));
        switchApproval.setOnCheckedChangeListener((b, v) -> Settings.setApprovalRequired(this, v));

        TextView txtApproved = findViewById(R.id.txtApproved);
        MaterialButton btnClearApproved = findViewById(R.id.btnClearApproved);
        refreshApproved();

        btnClearApproved.setOnClickListener(v -> {
            Settings.clearApproved(this);
            // 关键：还要撤销【当前已生效】的授权，否则音频照常发送，
            // 用户会以为"全部撤销"没生效。
            AudioServer srv = CaptureService.activeServer();
            if (srv != null) srv.revokeAll();
            refreshApproved();
        });

        // ---- 传输带宽 ----
        radioBandwidth = findViewById(R.id.radioBandwidth);
        for (int lv = 0; lv < Protocol.BW_COUNT; lv++) {
            MaterialRadioButton rb = findViewById(BW_IDS[lv]);
            rb.setText(Protocol.bwLabel(lv) + "　·　" + Protocol.bwFormatText(lv)
                    + "　·　" + Protocol.bwKbps(lv) + " kbps");
        }
        radioBandwidth.check(BW_IDS[Settings.bandwidth(this)]);
        radioBandwidth.setOnCheckedChangeListener((group, checkedId) -> {
            for (int lv = 0; lv < Protocol.BW_COUNT; lv++) {
                if (BW_IDS[lv] == checkedId) { Settings.setBandwidth(this, lv); break; }
            }
        });

        // ---- 源代码 ----
        TextView url = findViewById(R.id.txtSourceUrl);
        View rowSource = findViewById(R.id.rowSource);
        if (Settings.SOURCE_URL == null || Settings.SOURCE_URL.trim().isEmpty()) {
            // 仓库还没发布时不放虚假链接，如实显示
            url.setText(R.string.settings_source_unpublished);
            rowSource.setClickable(false);
        } else {
            url.setText(Settings.SOURCE_URL);
            rowSource.setOnClickListener(v -> openUrl(Settings.SOURCE_URL));
        }

        // ---- 版本 ----
        TextView ver = findViewById(R.id.txtVersion);
        ver.setText(getString(R.string.settings_version, versionName()));
    }

    /** 刷新"已批准的设备"清单 */
    private void refreshApproved() {
        TextView txt = findViewById(R.id.txtApproved);
        java.util.Set<String> set = Settings.approvedDevices(this);
        if (set.isEmpty()) {
            txt.setText(R.string.settings_approved_none);
            return;
        }
        // 按 IP 去重显示。历史版本曾把设备名也算进匹配条件，
        // 改名后会留下同一 IP 的多条过期记录，看起来就是"列表不正确"。
        java.util.Map<String, String> byAddr = new java.util.TreeMap<>();
        for (String k : set) {
            String a = Settings.addrOf(k);
            String n = Settings.nameOf(k);
            String prev = byAddr.get(a);
            if (prev == null || n.length() > prev.length()) byAddr.put(a, n);
        }
        StringBuilder sb = new StringBuilder();
        for (java.util.Map.Entry<String, String> e : byAddr.entrySet()) {
            sb.append("• ").append(e.getValue().isEmpty() ? "(未知设备)" : e.getValue())
              .append('\n').append("    ").append(e.getKey()).append('\n');
        }
        txt.setText(sb.toString().trim());
    }

    private void updateQualityHint(int q) {
        int ms = (q == Settings.Q_LOW_LATENCY) ? 20 : (q == Settings.Q_STABLE) ? 80 : 40;
        String t;
        if (q == Settings.Q_LOW_LATENCY) {
            t = getString(R.string.quality_hint_low, ms);
        } else if (q == Settings.Q_STABLE) {
            t = getString(R.string.quality_hint_stable, ms);
        } else {
            t = getString(R.string.quality_hint_balanced, ms);
        }
        txtQualityHint.setText(t + "\n\n" + getString(R.string.quality_note_lossless));
    }

    private String versionName() {
        try {
            return getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
        } catch (Exception e) {
            return "1.0";
        }
    }

    private void openUrl(String u) {
        try {
            startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(u)));
        } catch (ActivityNotFoundException e) {
            Toast.makeText(this, "没有可打开链接的应用", Toast.LENGTH_SHORT).show();
        }
    }

    @Override
    protected void onStart() {
        super.onStart();
        AppState.onActivityStart();
        refreshApproved();   // 每次进来都重新读，避免显示过期内容
    }

    @Override
    protected void onStop() {
        super.onStop();
        AppState.onActivityStop();
    }

    @Override
    protected void onPause() {
        super.onPause();
        // 失焦即保存名称，避免用户忘记
        String v = editName.getText() == null ? "" : editName.getText().toString().trim();
        Settings.setDeviceName(this, v);
        Settings.applyRuntime(this);   // 让日志开关等立即生效
    }
}
