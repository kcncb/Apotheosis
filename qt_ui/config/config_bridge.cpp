#include "config/config_bridge.h"
#include "config/ConfigManager.h"

#include <QSignalBlocker>
#include <QTimer>
#include <mutex>
#include <string>

#include "Apotheosis.h"
#include "config.h"
#include "capture.h"
#include "runtime/inference_session.h"
#include "runtime/config_snapshot.h"

extern std::atomic<bool> detector_model_changed;

ConfigBridge::ConfigBridge() : QObject(nullptr) {
    m_saveTimer = new QTimer(this);
    m_saveTimer->setSingleShot(true);
    m_saveTimer->setInterval(400);
    connect(m_saveTimer, &QTimer::timeout, this, &ConfigBridge::onSaveTimeout);

    connect(&ConfigManager::instance(), &ConfigManager::configChanged,
            this, &ConfigBridge::syncToRuntime);
}

ConfigBridge& ConfigBridge::instance() {
    static ConfigBridge s;
    return s;
}

void ConfigBridge::markDirty() {
    runtime_config::publish();
    if (!m_saveTimer->isActive())
        m_saveTimer->start();
}

void ConfigBridge::flush() {
    if (m_saveTimer->isActive())
        m_saveTimer->stop();
    onSaveTimeout();
}

void ConfigBridge::onSaveTimeout() {
    std::lock_guard<std::recursive_mutex> lk(configMutex);
    config.saveConfig();
}

void ConfigBridge::syncToRuntime() {
    auto& cm = ConfigManager::instance();
    std::lock_guard<std::recursive_mutex> lk(configMutex);

    auto qs = [](const QString& s) { return s.toStdString(); };

    // --- Capture: 只有「采集卡」一种方式 ---
    const std::string oldCaptureDevice = config.capture_device;
    const std::string oldCaptureFormat = config.capture_format;
    const int  oldCaptureWidth  = config.capture_width;
    const int  oldCaptureHeight = config.capture_height;
    const int  oldCaptureFps    = config.capture_fps;
    const bool oldCaptureGpu    = config.capture_gpu_decode;

    config.capture_device     = qs(cm.captureDevice());
    config.capture_format     = qs(cm.captureFormat());
    config.capture_width      = cm.captureWidth();
    config.capture_height     = cm.captureHeight();
    config.capture_fps        = cm.captureFps();
    config.capture_gpu_decode = cm.captureGpuDecode();

    // detection_resolution 与 circle_mask 都不再是界面选项:
    //   前者由模型输入边长推导 (见 inference_session.cpp publish_model_metadata),
    //   后者是固定设计。
    // 所以这里【绝不能】从 ConfigManager 回写 —— 那份副本只用于界面展示,
    // 一旦回写就会用陈旧值盖掉模型推导出来的真实尺寸。
    // oldDetRes 仍然快照, 因为下面靠它判断尺寸变化后要重建采集。
    const int oldDetRes = config.detection_resolution;

    // --- Hardware ---
    std::string oldInput = config.input_method;
    config.input_method      = qs(cm.inputMethod());
    config.makcu_baudrate = cm.makcuBaudrate();
    config.makcu_port     = qs(cm.makcuPort());
    config.makcu_new_baudrate = cm.makcuNewBaudrate();
    config.makcu_new_port     = qs(cm.makcuNewPort());
    config.kmbox_net_ip       = qs(cm.kmboxNetIp());
    config.kmbox_net_port     = qs(cm.kmboxNetPort());
    config.kmbox_net_uuid     = qs(cm.kmboxNetUuid());
    // --- AI ---
    std::string oldModel = config.ai_model;
    // ★ 2026-09-17: backend 恒为 TRT, dml_device_id 已随 DirectML 后端删除,
    //   max_detections 固定 kFixedMaxDetections —— 三者都不再从界面回写。
    config.backend              = "TRT";
    config.ai_model             = qs(cm.aiModel());
    config.confidence_threshold = cm.confidenceThreshold();
    config.nms_threshold        = cm.nmsThreshold();
    config.max_detections       = kFixedMaxDetections;
    config.small_target_enabled    = cm.smallTargetEnabled();
    config.small_target_confidence = cm.smallTargetConfidence();
    config.small_target_area_frac  = cm.smallTargetAreaFrac();


    // --- Overlay ---

    // --- Macro ---
    config.macro_enabled = cm.macroEnabled();
    config.macro_script_path = qs(cm.macroScriptPath());
    config.macro_primary_button_events = cm.macroPrimaryButtonEvents();

    // --- Crosshair ---
    config.crosshair_rect_w         = cm.crosshairRectW();
    config.crosshair_rect_h         = cm.crosshairRectH();
    config.crosshair_min_pixel_count = cm.crosshairMinPixelCount();
    config.crosshair_close_radius   = cm.crosshairCloseRadius();
    // (crosshair_smooth 已删除 2026-09-13)

    {
        auto qcolors = cm.crosshairColors();
        config.crosshair_colors.clear();
        for (const auto& qc : qcolors) {
            CrosshairColorProfileConfig c;
            c.name    = qs(qc.name);
            c.enabled = qc.enabled;
            c.h_low   = qc.hLow;
            c.h_high  = qc.hHigh;
            c.s_min   = qc.sMin;
            c.s_max   = qc.sMax;
            c.v_min   = qc.vMin;
            c.v_max   = qc.vMax;
            config.crosshair_colors.push_back(c);
        }
    }

    // --- Debug ---
    config.show_fps   = cm.showFps();
    config.verbose    = cm.verbose();
    config.screenshot_delay = cm.screenshotDelay();
    config.screenshot_button = { cm.screenshotButton().toStdString() };
    config.show_window      = cm.showWindow();
    config.replay_record_enabled = cm.replayRecordEnabled();
    config.replay_seconds        = cm.replaySeconds();
    config.replay_playback_speed = cm.replayPlaybackSpeed();

    // --- Active group & Hotkeys ---
    // Both are managed directly by HotkeyPage writing to config.hotkeys[]
    // and config.active_hotkey_group.  Syncing through ConfigManager risks
    // double-encoding CJK group names.  Do NOT overwrite here.

    // --- Set change flags ---
    // 采集参数任一变化都必须重建采集器 —— 采集卡路径不做任何热切换,
    // 因为格式/分辨率/帧率对不上时 MFCapture 会直接失败而不是降级。
    const bool captureDeviceChanged =
        config.capture_device != oldCaptureDevice
        || config.capture_format != oldCaptureFormat
        || config.capture_width  != oldCaptureWidth
        || config.capture_height != oldCaptureHeight
        || config.capture_fps    != oldCaptureFps
        || config.capture_gpu_decode != oldCaptureGpu;
    if (captureDeviceChanged)
        capture_method_changed = true;
    if (config.detection_resolution != oldDetRes)
        detection_resolution_changed = true;
    if (config.capture_fps != oldCaptureFps)
        capture_fps_changed = true;
    if (config.ai_model != oldModel) {
        detector_model_changed = true;
        std::string model_path = "models/" + config.ai_model;
        runtime::preload_model_metadata(model_path, false);
    }
    if (config.input_method != oldInput)
        input_method_changed = true;

    markDirty();
}

void ConfigBridge::syncFromRuntime()
{
    auto& cm = ConfigManager::instance();
    std::lock_guard<std::recursive_mutex> lk(configMutex);

    auto qstr = [](const std::string& s) { return QString::fromStdString(s); };

    QSignalBlocker blocker(&cm);

    // --- Capture: 只有「采集卡」一种方式 ---
    cm.setCaptureDevice(qstr(config.capture_device));
    cm.setCaptureFormat(qstr(config.capture_format));
    cm.setCaptureWidth(config.capture_width);
    cm.setCaptureHeight(config.capture_height);
    cm.setCaptureFps(config.capture_fps);
    cm.setCaptureGpuDecode(config.capture_gpu_decode);
    cm.setDetectionResolution(config.detection_resolution);
    cm.setCircleMask(config.circle_mask);

    // --- Hardware ---
    cm.setInputMethod(qstr(config.input_method));
    cm.setMakcuBaudrate(config.makcu_baudrate);
    cm.setMakcuPort(qstr(config.makcu_port));
    cm.setMakcuNewBaudrate(config.makcu_new_baudrate);
    cm.setMakcuNewPort(qstr(config.makcu_new_port));
    cm.setKmboxNetIp(qstr(config.kmbox_net_ip));
    cm.setKmboxNetPort(qstr(config.kmbox_net_port));
    cm.setKmboxNetUuid(qstr(config.kmbox_net_uuid));
    // --- AI ---
    // ★ 2026-09-17: setBackend / setDmlDeviceId 已删除(DirectML 后端整条移除);
    //   max_detections 固定, 但 setter 保留以维持既有调用序列。
    cm.setAiModel(qstr(config.ai_model));
    cm.setConfidenceThreshold(config.confidence_threshold);
    cm.setNmsThreshold(config.nms_threshold);
    cm.setMaxDetections(kFixedMaxDetections);
    cm.setSmallTargetEnabled(config.small_target_enabled);
    cm.setSmallTargetConfidence(config.small_target_confidence);
    cm.setSmallTargetAreaFrac(config.small_target_area_frac);


    // --- Overlay ---

    // --- Macro ---
    cm.setMacroEnabled(config.macro_enabled);
    cm.setMacroScriptPath(qstr(config.macro_script_path));
    cm.setMacroPrimaryButtonEvents(config.macro_primary_button_events);

    // --- Crosshair ---
    cm.setCrosshairRectW(config.crosshair_rect_w);
    cm.setCrosshairRectH(config.crosshair_rect_h);
    cm.setCrosshairMinPixelCount(config.crosshair_min_pixel_count);
    cm.setCrosshairCloseRadius(config.crosshair_close_radius);
    // (cm.setCrosshairSmooth 已删除 2026-09-13)
    {
        QList<ConfigManager::ColorProfile> qcolors;
        for (const auto& c : config.crosshair_colors) {
            ConfigManager::ColorProfile qc;
            qc.name    = qstr(c.name);
            qc.enabled = c.enabled;
            qc.hLow    = c.h_low;
            qc.hHigh   = c.h_high;
            qc.sMin    = c.s_min;
            qc.sMax    = c.s_max;
            qc.vMin    = c.v_min;
            qc.vMax    = c.v_max;
            qcolors.append(qc);
        }
        cm.setCrosshairColors(qcolors);
    }
    cm.setShowFps(config.show_fps);
    cm.setVerbose(config.verbose);
    cm.setScreenshotDelay(config.screenshot_delay);
    cm.setScreenshotButton(config.screenshot_button.empty()
        ? QStringLiteral("None")
        : QString::fromStdString(config.screenshot_button.front()));
    cm.setShowWindow(config.show_window);
    cm.setReplayRecordEnabled(config.replay_record_enabled);
    cm.setReplaySeconds(config.replay_seconds);
    cm.setReplayPlaybackSpeed(config.replay_playback_speed);

    // --- Active group ---
    cm.setActiveHotkeyGroup(qstr(config.active_hotkey_group));

    // --- Hotkeys ---
    for (int i = static_cast<int>(config.hotkeys.size()); i < cm.hotkeyCount(); )
        cm.removeHotkey(cm.hotkeyCount() - 1);
    for (int i = 0; i < static_cast<int>(config.hotkeys.size()); ++i) {
        const auto& hp = config.hotkeys[i];
        ConfigManager::HotkeyData hd;
        hd.name = qstr(hp.name);
        hd.group = qstr(hp.group);
        hd.keys.clear();
        for (const auto& k : hp.keys)
            hd.keys.push_back(qstr(k));
        hd.fovX = hp.fovX;
        hd.fovY = hp.fovY;
        // ── 【2026-09-17 整条删除】瞄准控制链的手键参数不再同步 ───────────────
        // 这里原本把 hp.trigger_* / hp.aim_path_* 等几十个字段整片抄进
        // ConfigManager::HotkeyData。HotkeyProfile 上那些字段已随瞄准控制链
        // (aim_pid / boss_aim / aim_scale / aim_path / auto_stop / trigger_scope /
        //  autotune_*)一起删除, 所以这些赋值也一并删除 —— 否则编不过。
        // ★ ConfigManager::HotkeyData 里对应的成员暂时【保留】: 它们是界面侧的
        //   本地结构, 不属于本次「配置层自洽」的范围; 但从此它们与 HotkeyProfile
        //   不再有任何连线(永远是结构体默认值), 只有 UI 页面自己读写。
        //   谁要清理它们, 应该连同页面上的控件一起做。
        //
        // 下面这些【保留】: 检测/瞄准点选择与准星找色这一侧还活着。
        {
            QString joined;
            for (size_t ai = 0; ai < hp.aim_classes.size(); ++ai) {
                if (ai > 0) joined.append(';');
                const auto& ac = hp.aim_classes[ai];
                joined.append(QString::number(ac.class_id));
                joined.append(':');
                joined.append(QString::number(ac.y_offset, 'f', 3));
                joined.append(':');
                joined.append(QString::number(ac.y_offset_max, 'f', 3));
                joined.append(':');
                joined.append(QString::number(ac.min_conf, 'f', 3));
            }
            hd.aimClasses = joined;
        }
        hd.crosshairDetectEnabled  = hp.crosshair_detect_enabled;
        hd.dynamicFovEnabled  = hp.dynamic_fov_enabled;
        hd.dynamicFovStrength = hp.dynamic_fov_strength;
        // ── ★★ 通用控制器层 (2026-09-17 第三轮续) ──────────────────────────
        // ★ 这一段的用途是「让界面控件按运行期真值重读」—— 铁律 (a) 要求
        //   每个改 config 的入口都刷新控件，否则界面会变成一个
        //   "随时把旧值灌回去的缓存"（HotkeyPage 当年就是这么出事的）。
        // ★★ 所以这里【必须】逐个赋值: 漏一个 = 那个控件永远显示旧值,
        //    用户一改别的控件就整片写回，把参数悄悄改回去、不报错不留痕。
        hd.ctlEnabled          = hp.ctl_enabled;
        hd.ctlKpX              = hp.ctl_kp_x;
        hd.ctlKpY              = hp.ctl_kp_y;
        hd.ctlKiX              = hp.ctl_ki_x;
        hd.ctlKiY              = hp.ctl_ki_y;
        hd.ctlKdX              = hp.ctl_kd_x;
        hd.ctlKdY              = hp.ctl_kd_y;
        hd.ctlTauUnwindSec     = hp.ctl_tau_unwind_sec;
        hd.ctlTauDerivSec      = hp.ctl_tau_deriv_sec;
        hd.ctlIMax             = hp.ctl_i_max;
        hd.ctlMaxOutputCounts  = hp.ctl_max_output_counts;
        hd.ctlPFullScalePx     = hp.ctl_p_full_scale_px;
        hd.ctlYOffset          = hp.ctl_y_offset;
        hd.ctlYOffsetMax       = hp.ctl_y_offset_max;
        hd.ctlHysteresisRatio  = hp.ctl_hysteresis_ratio;
        hd.ctlMaxDistancePx    = hp.ctl_max_distance_px;
        hd.ctlRandomSeed       = hp.ctl_random_seed;
        hd.ctlMatchCenterRatio = hp.ctl_match_center_ratio;
        hd.ctlAreaRatioTol     = hp.ctl_area_ratio_tol;
        hd.ctlKSnapMult        = hp.ctl_k_snap_mult;
        hd.ctlMinAspect        = hp.ctl_min_aspect;
        hd.ctlMaxAspect        = hp.ctl_max_aspect;
        // ★ 瞄准轨迹曲线 (aim_path_*) 与扳机 (trigger_*) 的同步已于 2026-09-17
        //   随 HotkeyProfile 上的字段一起删除 —— 那些字段没有消费者了。
        //   ConfigManager::HotkeyData 里的对应成员保持结构体默认值(见上)。
        if (i < cm.hotkeyCount())
            cm.setHotkey(i, hd);
        else
            cm.addHotkey(hd);
    }
}
