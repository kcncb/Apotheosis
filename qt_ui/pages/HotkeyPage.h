#pragma once

#include <QWidget>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QScrollArea;
class QSlider;
class QSpinBox;
class QVBoxLayout;

class BezierEditor;
class CardWidget;
class ToggleSwitch;

class HotkeyPage : public QWidget {
    Q_OBJECT

public:
    explicit HotkeyPage(QWidget* parent = nullptr);

private slots:
    void onProfileSelected(int row);
    void onAddProfile();
    void onDeleteProfile();
    void onCopyProfile();
    void onContextMenu(const QPoint& pos);
    void onRenameProfile();
    void onTrajectoryModeChanged(int index);
    void onBezierPreset(int presetIndex);
    void addKeyBindingRow(const QString& key = "None");
    void removeKeyBindingRow();

private:
    void buildLeftPanel(QWidget* parent);
    void buildRightPanel(QWidget* parent);
    void buildKeyBindingCard();
    void buildFovCard();
    void buildAimCard();
    void buildTrajectoryCard();
    void buildKalmanCard();
    void buildAdvancedCard();

    void addProfileItem(const QString& name, const QString& keyPreview);
    void restyleProfileItems();
    void updateProfileListItem(int row);
    void loadProfileToUi(int index);
    void saveUiToProfile(int index);
    void clearKeyBindings();
    void setBezierWidgetsVisible(bool visible);

    // Left panel
    QListWidget* m_profileList{};

    // Right panel scroll content
    QVBoxLayout* m_rightLayout{};

    // Card 1: Key bindings
    QLineEdit* m_profileName{};
    QVBoxLayout* m_keyBindingsLayout{};

    // Card 2: FOV
    QSlider* m_fovXSlider{};
    QSpinBox* m_fovXSpin{};
    QSlider* m_fovYSlider{};
    QSpinBox* m_fovYSpin{};
    ToggleSwitch* m_dynamicFov{};
    QDoubleSpinBox* m_dynamicFovMargin{};
    QDoubleSpinBox* m_dynamicFovMinRadius{};
    QWidget* m_dynamicFovContainer{};

    // Card 3: Aim
    QDoubleSpinBox* m_speedX{};
    QDoubleSpinBox* m_speedY{};
    QDoubleSpinBox* m_lockStrength{};
    QDoubleSpinBox* m_lockRadius{};
    // Smart trigger
    ToggleSwitch* m_smartTriggerEnable{};
    QDoubleSpinBox* m_smartTriggerHitRadius{};
    QDoubleSpinBox* m_smartTriggerVariance{};
    QSpinBox* m_smartTriggerWindow{};
    QSpinBox* m_smartTriggerDuration{};
    QWidget* m_smartTriggerContainer{};

    // Card 4: Trajectory
    QComboBox* m_trajectoryMode{};
    QWidget* m_bezierContainer{};
    BezierEditor* m_bezierEditor{};
    QDoubleSpinBox* m_followFactor{};
    QDoubleSpinBox* m_reanchorThreshold{};

    // Card 5: Kalman
    ToggleSwitch* m_kalmanEnable{};
    QDoubleSpinBox* m_kalmanPosNoise{};
    QDoubleSpinBox* m_kalmanVelNoise{};
    QDoubleSpinBox* m_kalmanMeasNoise{};
    QDoubleSpinBox* m_kalmanVelDecay{};
    QDoubleSpinBox* m_kalmanMaxVel{};
    QSpinBox* m_kalmanWarmup{};
    ToggleSwitch* m_kalmanCompensateDelay{};

    // Card 6: Advanced
    ToggleSwitch* m_crosshairDetect{};
    QDoubleSpinBox* m_lockSwitchMargin{};
    QSpinBox* m_lockSwitchMinFrames{};
    QSpinBox* m_lockHoldMinFrames{};
    QDoubleSpinBox* m_yOffsetDecayRate{};
    QSpinBox* m_yOffsetDecayDelay{};
    QDoubleSpinBox* m_threatPriorityWeight{};
    QDoubleSpinBox* m_threatDistanceWeight{};
};
