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
    void onAimClassFiltersChanged();

private:
    void buildLeftPanel(QWidget* parent);
    void buildRightPanel(QWidget* parent);
    void buildKeyBindCard();
    void buildFovCard();
    void buildCrosshairCard();
    void buildBossAimCard();
    void buildSmartTriggerCard();
    void buildAimPathCard();
    void buildAimClassCard();
    void buildAdvancedCard();

    void rebuildGroupCombo();
    void repopulateProfileList();
    void addProfileItem(int runtimeIndex);
    void restyleProfileItems();
    void loadProfileToUi(int runtimeIndex);
    void saveUiToCurrentProfile();

    void rebuildAimClassList();
    void rebuildAddClassCombo();

    int currentRuntimeIndex() const;
    std::vector<int> collectAimBucketClassIds();

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

    // Card 5: Aim class selection
    CardWidget* m_aimClassCard{};
    QVBoxLayout* m_aimClassListLayout{};
    QComboBox* m_addClassCombo{};
    QPushButton* m_addClassBtn{};

    // Card: Smart Trigger
    ToggleSwitch* m_smartTrigger{};
    QDoubleSpinBox* m_smartTriggerHitScale{};
    QDoubleSpinBox* m_smartTriggerAggression{};
    QSpinBox* m_smartTriggerHoldMs{};
    QSpinBox* m_smartTriggerCooldownMs{};

    // Card 6: Advanced
    QDoubleSpinBox* m_lockAggression{};

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
