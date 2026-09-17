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
    void onAddGroup();
    void onDeleteGroup();
    void onTargetClassesChanged();

private:
    void buildLeftPanel(QWidget* parent);
    void buildRightPanel(QWidget* parent);

    // 卡片（只留当前后端真的有的东西）
    void buildKeyBindCard();       // 触发按键
    void buildFovCard();           // 视野 FOV
    void buildAimClassCard();      // 瞄准类别（写 aim_classes）
    // ★★ 按旧外观重建的行列表（2026-09-17 第四轮续）:
    //   每行 = 优先级 #N + 类名 + ▲▼✕ + 随机锁点 Y 双 spin + 置信滑块。
    //   ★ 用 QWidget+QVBoxLayout 承载，不用 QListWidget+setItemWidget ——
    //     旧页明确试过后者，会压扁行 / 横向溢出 / 拖拽后留空行。
    void rebuildAimClassRows();
    void rebuildAddClassCombo();
    void moveAimClass(int from, int to);
    void buildCrosshairCard();     // 准星找色开关
    void buildControllerCard();    // ★★ 通用控制器层（23 个 ctl_*）
    void buildDynamicFovCard();    // 动态 FOV
    void buildTriggerCard();       // ★ 自动扳机（2026-09-17 恢复）
    void buildTrajectoryCard();    // ★ 轨迹曲线 / 风力曲线（2026-09-17 恢复）

    // ★ 小工具：hint 文案必须走 class 属性而不是 objectName ——
    //   theme.qss 的选择器是 QLabel[class="hint"]，objectName 匹配不到任何规则，
    //   那样 4 条提示会退化成无样式正文（旧页用的是 property）。
    static QLabel* makeHint(const QString& text);
    // 卡片内分段标题（旧页的 SectionTitle 用法）
    static QLabel* makeSectionTitle(const QString& text);
    // 一行 double 参数: 建 QDoubleSpinBox + 收集进 m_ctlDoubles + 套 fieldRow。
    // 抽出来是因为控制器卡有 19 个 double，逐个手写极易漏收进 m_ctlDoubles
    // (漏了就是"界面能改、写不回配置"的静默失效)。
    QWidget* makeDoubleRow(const char* obj, const char* label,
                           double lo, double hi, double step, double def);
    // ★ 带悬停说明的一行（2026-09-17）: 用户要求"每个参数都加上停留的参数说明"。
    //   tip 会同时设到行 widget 与里面的控件上 —— 鼠标停在标签或控件上都出提示。
    QWidget* makeIntRow(const char* obj, const char* label, int lo, int hi,
                        int step, int def, const QString& tip);
    QWidget* makeDoubleRowTip(const char* obj, const char* label, double lo, double hi,
                              double step, double def, const QString& tip);
    // 给一个已建好的行挂上 tip（含行内所有子控件）。
    static void attachTip(QWidget* row, const QString& tip);

    void rebuildGroupCombo();
    void rebuildProfileList();
    // 列表行的选中态配色（旧页 restyleProfileItems 的恢复）
    void restyleProfileItems();
    void reloadProfileToUi();
    void commitProfileFromUi();

    int currentRuntimeIndex() const;

    QVBoxLayout* m_rightLayout = nullptr;   // 卡片往这里加
    QComboBox*   m_groupCombo = nullptr;
    QListWidget* m_profileList = nullptr;
    QLabel*      m_leftTitle = nullptr;
    QStackedWidget* m_stack = nullptr;
    QLabel* m_emptyHint = nullptr;

    // 控件指针按卡片收集；reloadProfileToUi / commitProfileFromUi 逐个走。
    // ★ 用指针数组而不是"命名查找"，是为了让"漏一个"在代码里看得见。
    std::vector<QDoubleSpinBox*> m_ctlDoubles;
    std::vector<QSpinBox*>       m_ctlInts;
    // 自动扳机卡的 int 控件（2026-09-17 恢复）
    std::vector<QSpinBox*>       m_triggerInts;
    // 轨迹曲线卡的 int / double 控件（2026-09-17 恢复）
    std::vector<QSpinBox*>       m_pathInts;
    std::vector<QDoubleSpinBox*> m_pathDoubles;
    QLabel*                      m_pathSectionBezier = nullptr;

    // ★★ 瞄准类别卡的行容器（2026-09-17 第四轮续，按旧外观重建）。
    //   ★ 每次 reloadProfileToUi / 增删改都整段重建 —— 逐类参数是"按 classId
    //     索引的列表"，增删/换位后沿用旧控件指针极易错位。整段重建虽然笨，
    //     但"控件 ↔ classId"的对应关系每次都是新鲜的。
    QWidget*     m_aimClassContainer = nullptr;
    QVBoxLayout* m_aimClassLayout    = nullptr;
    QComboBox*   m_addClassCombo     = nullptr;
    QPushButton* m_addClassBtn       = nullptr;

    TargetPage* m_targetPage = nullptr;
    bool m_loading = false;
};
