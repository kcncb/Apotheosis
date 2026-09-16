#pragma once

// =============================================================================
// 「自动调参」页面 (靶场模式)
// =============================================================================
//
// ★ 2026-09-14 两次改版:
//   ①从"一个开关一直连调"改成【模式选择 + 开始/结束】, 每次只调一轮。
//     用户原话: "改成开始 结束按钮 然后加个模式选择 1.静态目标调参
//     2.动态目标调参 3.体感调参 也就是用户输入自己的体感来调参,
//     每次只调一轮"
//   ②采样边界从"按住瞄准热键"改成【两次按钮点击之间的日志】。
//     原因见 AutoTunePage.cpp 里 buildUi 的注释 —— 简单说: 按住热键那条路
//     配套的判据读错了 `target_switched` 的语义(它其实是"玩家甩枪"),
//     导致几乎每一段都被判成"中途换了目标"。现在按按钮切段, 不读引擎信号。
//
// 这一页做五件事:
//   ① LLM 配置(base_url / api_key / model) —— 填一次
//   ② 模式选择: 静态 / 动态 / 体感
//   ③ 开始 / 结束 —— 采两次点击之间的日志, 结束才调一轮
//   ④ 状态显示 —— 当前参数 / 本轮指标 / 模型说了什么
//   ⑤ 一键回退 —— 退回"开始调参之前"的那组参数
//   另有一组「距离尺度基准」(与调参会话无关, 随时可用)
//
// ★ 为什么没有自动连调:
//   连调十轮你分不清是哪一轮起的作用, 而且中间几轮的数据是"你还在适应新
//   参数"的过渡态。调参的节奏必须是人的节奏: 调一轮 → 你打几枪 → 用体感
//   判断 → 再调一轮。
//
// ★ 安全提示必须显眼: 这一页会【真的改你正在用的参数】, 所以
//   · 明确写出"只在靶场用"
//   · 每一步改动都显示在历史里(含夹取说明与回滚原因)
//   · 提供一键回退
// =============================================================================

#include <QWidget>

#include <memory>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QDoubleSpinBox;
class QTableWidget;
class QTimer;

namespace boss::autotune { struct Round; }

class AutoTunePage : public QWidget
{
    Q_OBJECT

public:
    explicit AutoTunePage(QWidget* parent = nullptr);
    ~AutoTunePage() override;

private slots:
    void onBeginSession();      // 「开始」: 打开采样会话
    void onEndSession();        // 「结束」: 切段 + 调一轮
    void onModeChanged(int idx);
    // ★ 本轮改动幅度(保守/平稳/激进)。改了立刻推给 runtime + 持久化 ——
    //   它决定 AI 这一轮能改多大, 必须在点「结束」之前就已经生效。
    void onStepStyleChanged(int idx);
    void onSetBaseline();
    void onRestore();
    void onTestConnection();
    void onRefresh();          // 定时刷新状态与历史

private:
    void buildUi();
    void loadSettings();
    void saveSettings();
    void appendRound(const boss::autotune::Round& r);
    // 把当前界面上的接口配置同步给 runtime
    void pushLlmConfig();
    // 让"开始/结束/模式/感受框"的可用状态跟会话状态一致
    void syncSessionControls();

    QLineEdit* m_baseUrl{};
    QLineEdit* m_apiKey{};
    QLineEdit* m_model{};
    QSpinBox*  m_timeoutMs{};
    // ★ 最大输出 token。推理模型的【思考】也占这个预算, 太小就会
    //   只剩思考、没有正文 —— 症状是"测试连接成功但没 content"。
    QSpinBox*  m_maxTokens{};

    // 调参模式 + 采样会话
    QComboBox* m_mode{};
    QLabel* m_modeHint{};
    // 本轮改动幅度: 保守(±10%) / 平稳(±20%) / 激进(±50%)
    QComboBox* m_stepStyle{};
    QLabel*    m_stepHint{};
    QLabel* m_feelLabel{};
    QPlainTextEdit* m_feelText{};
    QPushButton* m_begin{};
    QPushButton* m_end{};
    QPushButton* m_restore{};

    // 距离尺度基准(与调参会话无关)
    QPushButton* m_setBaseline{};
    QLabel* m_baselineInfo{};

    QPushButton* m_testBtn{};
    QLabel* m_testResult{};

    QLabel* m_status{};
    QLabel* m_currentParams{};
    QTableWidget* m_history{};
    QPlainTextEdit* m_detail{};

    QTimer* m_timer{};
    int m_shownRounds = 0;
    // 已经弹过提示的轮次号。★ 防重复弹窗: 详情/状态刷新是周期性的,
    // 没有这个标记的话同一次失败会被反复弹出来。
    int m_lastNotifiedRound = 0;
};
