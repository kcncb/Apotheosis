#pragma once

#include <QWidget>
#include <QString>
#include <vector>

#include "capture/capture_card_caps.h"

class QComboBox;
class QLabel;
class QPushButton;
class QVBoxLayout;
class ToggleSwitch;
class CardWidget;

// 采集设置页。
//
// 这里【只有一种采集方式: 采集卡】。所有参数都来自设备真实能力探测结果, 用
// 格式 / 分辨率 / 帧率 三级联动下拉让用户从中选 —— 设备不支持的组合
// 【根本不会出现在下拉里】, 而不是让用户选完再悄悄降级。
//
// 例: 某卡的 NV12 只到 1080p60 而 MJPG 能到 1080p240, 那么选 NV12 时
//     帧率下拉里只有 60; 换成 MJPG 才会出现 240。
//
// 另外, 全程【没有任何回退】: 上次选的卡没插就不选任何卡并提示, 不会自动
// 换一张; 配置里的组合失效就提示并让用户重选, 不会"吸附"到别的模式。
class CapturePage : public QWidget {
    Q_OBJECT

public:
    explicit CapturePage(QWidget* parent = nullptr);

private slots:
    void onLoadConfig();
    void refreshDevices();
    void onDeviceChanged(int index);
    void onFormatChanged(int index);
    void onResolutionChanged(int index);
    void onFpsChanged(int index);

private:
    // 把三级下拉的当前选择写回配置 (只有通过设备能力校验才会写)。
    void applySelectionToConfig();

    void buildCardCard(QVBoxLayout* layout);

    // 三级联动: 逐级收缩, 每一级都只列设备真实支持的项。
    void rebuildFormatCombo();
    void rebuildResolutionCombo();
    void rebuildFpsCombo();

    const MFDeviceInfo* currentDevice() const;
    void updateCapabilitySummary();
    void showError(const QString& text);
    void clearError();

    // ── 采集卡 ──
    CardWidget*   m_cardCard{};
    QComboBox*    m_devCombo{};
    QComboBox*    m_fmtCombo{};
    QComboBox*    m_resCombo{};
    QComboBox*    m_fpsCombo{};
    ToggleSwitch* m_gpuDecode{};
    QLabel*       m_capSummary{};
    QLabel*       m_recommend{};
    QLabel*       m_error{};
    QPushButton*  m_refreshBtn{};

    std::vector<MFDeviceInfo> m_devices;

    // 正在按配置还原三级下拉。
    //
    // 还原过程中绝不允许回写配置: refreshDevices()/onLoadConfig() 会逐级重建
    // 下拉, 中间态是"这一级还没选中"。若此时落盘, 落下的就是中间态的替身值
    // —— 实测会把用户存的 NV12 1080p120 悄悄改成 NV12 3840x2160@25, 而用户
    // 只是打开了程序。回写只应由用户在三级的显式操作触发。
    bool m_restoring = false;
};
