#pragma once

#include <QWidget>

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
private:
    void buildLeftPanel(QWidget* parent);
    void buildRightPanel(QWidget* parent);
    void buildKeyBindCard();
    void buildFovCard();
    void buildCrosshairCard();
    void buildBossAimCard();
    void buildDeadzoneCard();
    void buildTriggerCard();
    void buildTargetSelectionCard();
    void buildAimPathCard();

    void rebuildGroupCombo();
    void repopulateProfileList();
    void addProfileItem(int runtimeIndex);
    void restyleProfileItems();
    void loadProfileToUi(int runtimeIndex);
    void saveUiToCurrentProfile();

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
    QDoubleSpinBox* m_dynamicFovMinRadius{};
    QWidget* m_dynamicFovContainer{};

    // Card 3: Crosshair / Laser / Flashlight detect + Glass filter
    ToggleSwitch* m_crosshairDetect{};
    ToggleSwitch* m_laserDetect{};
    ToggleSwitch* m_flashlightDetect{};
    ToggleSwitch* m_glassFilter{};

    // Card 4: Mover (kind dropdown + stacked per-mover params)
    QComboBox*      m_moverKindCombo{};
    QStackedWidget* m_moverParamStack{};
    // 微澜 (Smooth) — ART 原 3 参数
    QDoubleSpinBox* m_speedXSpin{};
    QDoubleSpinBox* m_speedYSpin{};
    QDoubleSpinBox* m_deadZoneSpin{};
    // 疾风 (Predictive) — 4 项
    QDoubleSpinBox* m_predKpXSpin{};
    QDoubleSpinBox* m_predKpYSpin{};
    QDoubleSpinBox* m_predKdSpin{};
    QDoubleSpinBox* m_predPwSpin{};
    // 天枢 (Classic) — 经典 PID 全参
    QComboBox*      m_clsAimModeCombo{};
    QStackedWidget* m_clsAimModeStack{};
    // 简单模式
    QDoubleSpinBox* m_clsStartSpeed{};
    QDoubleSpinBox* m_clsEndSpeed{};
    QSpinBox*       m_clsTransitionMs{};
    QDoubleSpinBox* m_clsSimpleKi{};
    QDoubleSpinBox* m_clsSimpleKd{};
    // 高级模式 X
    QDoubleSpinBox* m_clsKpMinX{};
    QDoubleSpinBox* m_clsKpMaxX{};
    QDoubleSpinBox* m_clsKiX{};
    QDoubleSpinBox* m_clsKdX{};
    QDoubleSpinBox* m_clsImaxX{};
    QDoubleSpinBox* m_clsPfactorX{};
    QSpinBox*       m_clsTimeX{};
    ToggleSwitch*   m_clsTimeDynX{};
    // 高级模式 Y
    QDoubleSpinBox* m_clsKpMinY{};
    QDoubleSpinBox* m_clsKpMaxY{};
    QDoubleSpinBox* m_clsKiY{};
    QDoubleSpinBox* m_clsKdY{};
    QDoubleSpinBox* m_clsImaxY{};
    QDoubleSpinBox* m_clsPfactorY{};
    QSpinBox*       m_clsTimeY{};
    ToggleSwitch*   m_clsTimeDynY{};
    // 预测
    QComboBox*      m_clsPredModeCombo{};
    QDoubleSpinBox* m_clsVelLead{};
    ToggleSwitch*   m_clsIndependentY{};
    // Kalman
    QDoubleSpinBox* m_clsKalmanQPos{};
    QDoubleSpinBox* m_clsKalmanQVel{};
    QDoubleSpinBox* m_clsKalmanRObs{};
    QDoubleSpinBox* m_clsKalmanLookahead{};
    QWidget*        m_clsKalmanContainer{};
    // Card: 死区 (shared deadzone)
    ToggleSwitch*   m_deadzoneEnabled{};
    QSlider*        m_deadzonePercent{};

    // Card: 扳机 (trigger FSM)
    ToggleSwitch*   m_triggerEnabled{};
    QSpinBox*       m_triggerFireDelay{};
    QSpinBox*       m_triggerFireDuration{};
    QSpinBox*       m_triggerFireInterval{};
    QSpinBox*       m_triggerYPercent{};

    // Card: 目标选择 (3-slot target selection)
    QSpinBox*       m_targetClass1{};
    QSlider*        m_targetYTop1{};
    QSlider*        m_targetYBot1{};
    QSlider*        m_targetMinConf1{};
    QSpinBox*       m_targetClass2{};
    QSlider*        m_targetYTop2{};
    QSlider*        m_targetYBot2{};
    QSlider*        m_targetMinConf2{};
    QSpinBox*       m_targetClass3{};
    QSlider*        m_targetYTop3{};
    QSlider*        m_targetYBot3{};
    QSlider*        m_targetMinConf3{};
    QSpinBox*       m_targetAimRange{};

    // Card 7: Aim trajectory
    QButtonGroup*       m_aimPathModeGroup{};
    QRadioButton*       m_aimPathModeLinear{};
    QRadioButton*       m_aimPathModeBezier{};
    QRadioButton*       m_aimPathModeCustom{};
    QStackedWidget*     m_aimPathEditorStack{};
    BezierEditor*       m_aimPathBezier{};
    FreehandCurveEditor* m_aimPathFreehand{};

    bool m_loading{false};
};
