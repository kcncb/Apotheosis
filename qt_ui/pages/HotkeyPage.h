#pragma once

#include <QWidget>
#include <array>

class QCheckBox;

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QPushButton;
class QScrollArea;
class QSlider;
class QSpinBox;
class QTimer;
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
    // 【2026-09-13 删除】refreshCalibration() —— 「每计数像素」的显示刷新。
    // 前馈移除后不再需要, 连同定时器一起删除。
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
    void buildTrajectoryCard();

    void rebuildGroupCombo();
    void repopulateProfileList();
    void addProfileItem(int runtimeIndex);
    void restyleProfileItems();
    void loadProfileToUi(int runtimeIndex);
    void saveUiToCurrentProfile();

    // 把 config 里当前档位的值整片重读到控件上 (configLoaded 时调用)。
    // ★ 存在的理由: 本页的写回是"控件值整片写回 config", 所以一旦
    //   config 被外部改过(自动调参 / live_tune)而控件没跟着重读,
    //   用户下次改任何控件都会把陈旧值回灌 —— 参数看起来被改回去了。
    void reloadProfileFromConfig();

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
    // [4][5] 过冲控制 Kd;
    // 整数行: 移动死区 X/Y(像素)、移动限幅 X/Y(计数/拍)。
    //
    // 2026-09-13: 原 [6..9](提前量 / 延迟预测) 两个旋钮已删除 —— 它们是
    // "目标速度 × 可调时间"的固定偏移, 会让准星稳定停在目标的【下一拍位置】
    // 而不是目标身上。延迟补偿现在由控制器用【实测死区】与在途自身位移配对完成
    // (扣自己的位移 + 补敌人的位移, 稳态抵消), 是物理量, 不该由用户调。
    std::array<QDoubleSpinBox*, 6> m_pidfGain{};
    std::array<QSpinBox*, 4> m_pidfInteger{};
    // 在途补偿强度 beta X/Y (2026-09-14 恢复)。★ 这是控制器最关键的一个旋钮:
    // 0 不是"关掉可选优化", 而是拆掉主刹车(实测尾段变几百像素极限环)。
    std::array<QDoubleSpinBox*, 2> m_pidfInflight{};
    // 尺度调度 s(bbox.height) —— 按框高相对【基准】的比值调制等效增益。
    // ★ 2026-09-14 改版: 删掉"近处框高/远处框高"两个绝对值输入框(用户测不出当前
    //   框高, 换游戏/分辨率也失效)。基准在「自动调参」页按按钮设定, 本页只读显示。
    QCheckBox* m_scaleEnabled{};      // 启用尺度调度
    QDoubleSpinBox* m_scaleMax{};     // 近处上限 (1.0~2.0)
    QDoubleSpinBox* m_scaleMin{};     // 远处下限 (0.30~1.00)
    QLabel* m_scaleBaseLabel{};       // 基准框高: 只读显示(在「自动调参」页按按钮设定)
    // 预测补偿 (2026-09-13 重做, 对齐 AimMagic 1.0.30「预测补偿」页)
    std::array<QDoubleSpinBox*, 2> m_predictFactor{};   // 预测系数 X/Y
    QSpinBox* m_predictMinW{};                          // 最小预测宽度 (px)
    QSpinBox* m_predictMaxW{};                          // 最大预测宽度 (px)
    QDoubleSpinBox* m_predictDamp{};                    // 方向翻转阻尼
    QSpinBox* m_predictMaxPx{};                         // 提前量硬上限 (px, 0=内置12)
    QSpinBox* m_predictVelFloor{};                      // 速度噪声门 (px/s)
    // ── 跟踪器 / 预测 (2026-09-15 新增, 2026-09-16 按 AM 逐字移植重写) ────────
    // 键集合与 AimMagic 1.0.30 的 Group 作用域一一对应, 见 config.h 的长注释与
    // docs/aimmagic-ground-truth.md。★ 删掉的那些控件(关联距离/每计数像素/
    // 在途换算窗/自运动补偿)在 AM 里【没有对应】或属于已整条删除的像素域链。
    // (档位下拉 m_aimMode 已于 2026-09-16 随「经典 PID」档一起删除。)
    QSpinBox*       m_esyncMinHits{};       // 累计命中数下限 (AM min_hits = 3)
    QSpinBox*       m_esyncMaxAge{};        // 滑行帧数上限 (AM max_age = 5)
    QDoubleSpinBox* m_esyncIoU{};           // 关联 IoU 门限 (AM = 0.30, 严格大于)
    QSpinBox*       m_esyncVelSample{};     // 速度采样窗 ms (AM = 20, 持有语义)
    // AM 的 prediction_factor_x/y(无夹取) + prediction_min_width / max_width。
    QDoubleSpinBox* m_esyncPredFactorX{};
    QDoubleSpinBox* m_esyncPredFactorY{};
    QSpinBox*       m_esyncPredMinW{};      // 尺寸权重下限(框高 <= 它时权重保持 1.0)
    QSpinBox*       m_esyncPredMaxW{};      // 尺寸权重上限(框高 >= 它时权重归 0)
    // 【2026-09-13 删除】「每计数像素」的两个输入框 + 「测量」按钮 + 状态标签 +
    // 刷新定时器 (m_pxPerCount / m_measureBtn / m_measureStatus / m_measureTimer)。
    // 它们服务的 k̂ 只有前馈才需要, 而前馈已整条移除 —— 现在没有东西可填、可量了。
    // 框平滑改用 mouse/anchor_filter.h 的 α-β 滤波器, 紧挨在 PID 之前, 参数是
    // 编译期常数 kAnchorFilterTauMs。

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
    // 自动开镜 (仿 AimMagic 的「开火方式」): 0 关 / 1 点按右键 / 2 长按右键。
    QComboBox*      m_triggerAutoScope{};
    QSpinBox*       m_triggerScopeDelay{};
    // 自动急停 (开火时补一个反方向键; 只有 MAKCUNEW 有键盘通道)
    ToggleSwitch*   m_triggerAutoStop{};
    QSpinBox*       m_triggerStopMs{};
    class TriggerVisualWidget* m_triggerVisual{};

    // Card: 目标选择 (优先级排序列表)
    CardWidget*  m_aimClassCard{};
    QWidget*     m_aimClassContainer{};  // 承载行卡片的容器
    QVBoxLayout* m_aimClassLayout{};     // 行卡片纵向布局, 顺序 = 优先级
    QComboBox*   m_addClassCombo{};   // "+ 添加" 下拉候选 (来源: Target 页 Aim 桶)
    QPushButton* m_addClassBtn{};

    // Card: 轨迹曲线 (mouse/aim_path.h)
    //   模式: 0 直线 / 1 贝塞尔 / 2 自定义 / 3 WindMouse(仿 AM 的 enable_mouse_curve)
    //   WindMouse 的 G0/W0/M0/D0 与 AM 的 wind_mouse_* 同名同量纲; 阈值 = AM 的
    //   curve_threshold (两轴误差都不超过它时整段曲线旁路)。
    QComboBox*      m_aimPathMode{};
    QSlider*        m_aimPathInfluenceSlider{};
    QSpinBox*       m_aimPathInfluenceSpin{};
    QWidget*        m_windMouseContainer{};
    QDoubleSpinBox* m_windGravity{};
    QDoubleSpinBox* m_windWind{};
    QDoubleSpinBox* m_windStep{};
    QDoubleSpinBox* m_windDistance{};
    QSpinBox*       m_windThreshold{};

    bool m_loading{false};
};
