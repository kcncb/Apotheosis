#pragma once

#include <QStringList>
#include <QWidget>

class QButtonGroup;
class QComboBox;
class QHBoxLayout;
class QPushButton;
class QTimer;
class QToolButton;
class StatusPill;

// 应用外壳顶部栏:品牌 + 主导航(下划线指示)+ 全局操作(配置方案 / 状态 / 保存)。
class TopNavBar : public QWidget {
    Q_OBJECT

public:
    explicit TopNavBar(QWidget* parent = nullptr);

    void setPrimaryItems(const QStringList& labels);
    void setCurrentPrimary(int index);
    int currentPrimary() const;
    void setSessionStatus(bool running, const QString& text);
    void showSaveFeedback();

    // 全局配置方案列表。active 为当前生效方案名 (不在列表里时按未选中显示)。
    // 填充期间屏蔽信号, 不会反过来触发切换。
    void setProfiles(const QStringList& names, const QString& active);
    QString currentProfile() const;
    void setProfileControlsEnabled(bool enabled);
    void showProfileFeedback(const QString& text);

signals:
    void primaryChanged(int index);
    void saveClicked();

    // ── 全局配置方案 ──
    void profileSwitchRequested(const QString& name);
    void profileSaveRequested();
    void profileSaveAsRequested();
    void profileRenameRequested();
    void profileDeleteRequested();
    void profileOpenDirRequested();
    void profileRefreshRequested();

private:
    QButtonGroup* m_group{};
    QHBoxLayout* m_navRow{};
    StatusPill* m_status{};
    QComboBox* m_profileCombo{};
    QToolButton* m_profileMenuBtn{};
    QString m_profileTip;
    QPushButton* m_saveButton{};
    QTimer* m_saveFeedbackTimer{};
    QTimer* m_profileFeedbackTimer{};
};

