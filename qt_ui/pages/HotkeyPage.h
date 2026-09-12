#pragma once

#include <QWidget>
#include <array>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QPushButton;
class QScrollArea;
class QSlider;
class QSpinBox;
class QVBoxLayout;

class AdaptiveStack;
class CardWidget;
class QButtonGroup;
class QRadioButton;
class QStackedWidget;
class TargetPage;
class ToggleSwitch;

class HotkeyPage : public QWidget {
    Q_OBJECT

public:
    explicit HotkeyPage(QWidget* parent = nullptr);

    void setTargetPage(TargetPage* tp);

protected:
    void showEvent(QShowEvent* event) override;

public slots:
    void reloadFromRuntime();

private slots:
    void onGroupChanged(int index);
    void onProfileSelected(int row);
    void onAddProfile();
    void onDeleteProfile();
    void onCopyProfile();
    void onAddGroup();
    void onDeleteGroup();
    void onContextMenu(const QPoint& pos);
    void onRenameProfile();
    void onAimClassFiltersChanged();  // Target 页把某类切换成 Aim 桶时被调,刷新可选下拉+当前列表
private:
    void buildLeftPanel(QWidget* parent);
    void buildRightPanel(QWidget* parent);
    void buildKeyBindCard();
    void buildFovCard();
    void buildCrosshairCard();
    void buildBossAimCard();
    void buildTriggerCard();
    void buildAimClassCard();

    void rebuildGroupCombo();
    void repopulateProfileList();
    void addProfileItem(int runtimeIndex);
    void restyleProfileItems();
    void loadProfileToUi(int runtimeIndex);
    void saveUiToCurrentProfile();

    void rebuildAimClassList();       // 依 config.hotkeys[ri].aim_classes 重画列表
    void rebuildAddClassCombo();      // 依 config.class_filters 里桶=Aim 且未加入的类别刷新下拉
    void moveAimClass(int from, int to);  // ▲▼ 换位: 交换 aim_classes 两项后整表重建

    int currentRuntimeIndex() const;

    TargetPage* m_targetPage{};

    // Left panel
    QComboBox* m_groupCombo{};
    QListWidget* m_profileList{};
    QLabel* m_leftTitle{};

    // Right panel scroll content
    QVBoxLayout* m_rightLayout{};

    // Card: 触发按键 (single-key dropdown)
    QComboBox* m_keyCombo{};

    // Card: FOV
    QSlider* m_fovXSlider{};
    QSpinBox* m_fovXSpin{};
    QSlider* m_fovYSlider{};
    QSpinBox* m_fovYSpin{};
    ToggleSwitch* m_dynamicFov{};
    QDoubleSpinBox* m_dynamicFovMargin{};
    QWidget* m_dynamicFovContainer{};

    // Card 3: Crosshair detect
    ToggleSwitch* m_crosshairDetect{};

    // 控制器参数(mouse/aim_pid.h): [0][1] 瞄准速度 Kp  [2][3] 积分强度 Ki
    // [4][5] 过冲控制 Kd  [6][7] 提前量(秒)  [8][9] 延迟预测(秒);
    // 整数行: 移动死区 X/Y(像素)、移动限幅 X/Y(计数/拍)。
    std::array<QDoubleSpinBox*, 10> m_pidfGain{};
    std::array<QSpinBox*, 4> m_pidfInteger{};
    // 「每计数像素」X/Y: 手填就用, 0 = 自动估算(见 config.h 的 aim_px_per_count_*)。
    std::array<QDoubleSpinBox*, 2> m_pxPerCount{};
    // 瞄点滤波(anchor_filter_ms) 已于 2026-09-12 移除: 位置不再平滑, 见 boss_aim.h。

    // Card: 扳机 (trigger FSM)
    ToggleSwitch*   m_triggerEnabled{};
    QSpinBox*       m_triggerFireDelay{};
    QSpinBox*       m_triggerFireDuration{};
    QSpinBox*       m_triggerFireInterval{};
    QSpinBox*       m_triggerYPercent{};
    QSpinBox*       m_triggerDelayJitter{};
    QSpinBox*       m_triggerDurationJitter{};
    QSpinBox*       m_triggerIntervalJitter{};
    QSpinBox*       m_triggerSwitchCooldown{};
    class TriggerVisualWidget* m_triggerVisual{};

    // Card: 目标选择 (优先级排序列表)
    CardWidget*  m_aimClassCard{};
    QWidget*     m_aimClassContainer{};  // 承载行卡片的容器
    QVBoxLayout* m_aimClassLayout{};     // 行卡片纵向布局, 顺序 = 优先级
    QComboBox*   m_addClassCombo{};   // "+ 添加" 下拉候选 (来源: Target 页 Aim 桶)
    QPushButton* m_addClassBtn{};

    bool m_loading{false};
};
