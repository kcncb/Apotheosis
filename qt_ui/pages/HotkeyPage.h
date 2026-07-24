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

class BezierEditor;
class AdaptiveStack;
class CardWidget;
class FreehandCurveEditor;
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
    void buildAimPathCard();
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

    // Card 3: Crosshair / Laser / Flashlight detect + Glass filter
    ToggleSwitch* m_crosshairDetect{};
    ToggleSwitch* m_laserDetect{};
    ToggleSwitch* m_flashlightDetect{};
    ToggleSwitch* m_glassFilter{};

    // AVA PIDF Mode 1：Kp/Ki/Kd/Kf/Lr 各 XY，及移动死区/限幅。
    std::array<QDoubleSpinBox*, 10> m_pidfGain{};
    std::array<QSpinBox*, 4> m_pidfInteger{};
    QSpinBox*       m_lostTargetCacheFrames{};

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

    // Card: 目标选择 (优先级排序列表)
    CardWidget*  m_aimClassCard{};
    QWidget*     m_aimClassContainer{};  // 承载行卡片的容器
    QVBoxLayout* m_aimClassLayout{};     // 行卡片纵向布局, 顺序 = 优先级
    QComboBox*   m_addClassCombo{};   // "+ 添加" 下拉候选 (来源: Target 页 Aim 桶)
    QPushButton* m_addClassBtn{};

    // Card 7: Aim trajectory
    QButtonGroup*       m_aimPathModeGroup{};
    QRadioButton*       m_aimPathModeLinear{};
    QRadioButton*       m_aimPathModeBezier{};
    QRadioButton*       m_aimPathModeCustom{};
    QSpinBox*           m_aimPathInfluence{};
    QStackedWidget*     m_aimPathEditorStack{};
    BezierEditor*       m_aimPathBezier{};
    FreehandCurveEditor* m_aimPathFreehand{};
    bool m_neuralCurveActive = false;
    std::array<float, 25> m_neuralCurveWeights{};

    bool m_loading{false};
};
