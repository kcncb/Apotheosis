#pragma once

// 热键与瞄准设置页（2026-09-17 第三轮续重建）
//
// ★★ 为什么重建: 上一轮把整条瞄准控制链删掉时，这一页（HotkeyPage）也一起删了。
//    但那个删除【删过头了】—— 它带走的不只是"死掉的瞄准参数"，还包括：
//      · 热键列表的管理（增/删/复制/分组）
//      · `aim_classes` 类别优先级列表的编辑
//      · 准星找色开关、动态 FOV
//    而这些东西背后是【活着的】子系统。
//    ★ 后果是 `config.hotkeys[]` 从那时起【没有任何写入者】——
//      它只在启动时从 ini 读一次，界面再也改不动它。
//      本页的职责就是把那个写入者补回来。
//
// ★ 与旧页的区别（这是"改成当前后端实现的参数"那一条）:
//    旧页右栏的 `触发卡 / BossAim卡 / 轨迹曲线卡 / 尺度调度卡` 全部删除 ——
//    它们描述的是已经不存在的那条链（PID-EventSync / 尺度调度 / 轨迹整形）。
//    换成本项目【当前真的在跑】的通用控制器层参数（docs/generic-controller-layer.md）。

#include <QWidget>

#include <vector>

class QCheckBox;
class QComboBox;
class QVBoxLayout;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTimer;

class CardWidget;
class TargetPage;

class AimSettingsPage : public QWidget
{
    Q_OBJECT

public:
    explicit AimSettingsPage(QWidget* parent = nullptr);

    void setTargetPage(TargetPage* tp);

protected:
    void showEvent(QShowEvent* event) override;

public slots:
    // ★★ 铁律 (a): 任何外部改 config 的入口（方案切换 / 调参 / live_tune）
    //    都必须让本页【重读控件】。本页是把控件整片写回 config 的那种页面，
    //    不重读的话它就不是"显示"，而是一个随时会把旧值灌回去的缓存。
    void reloadFromRuntime();

private slots:
    void onGroupChanged(int index);
    void onProfileSelected(int row);
    void onAddProfile();
    void onDeleteProfile();
    void onCopyProfile();
    void onTargetClassesChanged();

private:
    void buildLeftPanel(QWidget* parent);
    void buildRightPanel(QWidget* parent);

    // 卡片（只留当前后端真的有的东西）
    void buildKeyBindCard();       // 触发按键
    void buildFovCard();           // 视野 FOV
    void buildAimClassCard();      // 瞄准类别（写 aim_classes）
    void buildCrosshairCard();     // 准星找色开关
    void buildControllerCard();    // ★★ 通用控制器层（23 个 ctl_*）
    void buildDynamicFovCard();    // 动态 FOV

    void rebuildGroupCombo();
    void rebuildProfileList();
    void reloadProfileToUi();
    void commitProfileFromUi();

    int currentRuntimeIndex() const;

    QVBoxLayout* m_rightLayout = nullptr;   // 卡片往这里加
    QComboBox*   m_groupCombo = nullptr;
    QListWidget* m_profileList = nullptr;
    QStackedWidget* m_stack = nullptr;
    QLabel* m_emptyHint = nullptr;

    // 控件指针按卡片收集；reloadProfileToUi / commitProfileFromUi 逐个走。
    // ★ 用指针数组而不是"命名查找"，是为了让"漏一个"在代码里看得见。
    std::vector<QDoubleSpinBox*> m_ctlDoubles;
    std::vector<QSpinBox*>       m_ctlInts;

    TargetPage* m_targetPage = nullptr;
    bool m_loading = false;
};
