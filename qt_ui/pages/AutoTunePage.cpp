// =============================================================================
// 「自动调参」页面实现
// =============================================================================

#include "pages/AutoTunePage.h"

#include "mouse/autotune_agent.h"
#include "mouse/autotune_runtime.h"

#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QApplication>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <thread>

#include <cstdio>

namespace {

QString fromUtf8(const char* s) { return QString::fromUtf8(s); }

} // namespace

AutoTunePage::AutoTunePage(QWidget* parent) : QWidget(parent)
{
    buildUi();
    loadSettings();

    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &AutoTunePage::onRefresh);
    m_timer->start(500);   // 半秒刷一次状态, 够用且不吵
}

AutoTunePage::~AutoTunePage()
{
    // ★ 2026-09-14 起调参是"开始/结束"式的采样会话, 没有后台线程。
    //   关页面时唯一要保证的是【没在采样中途被切走】—— 会话本身只是个标记,
    //   真正改参数只发生在「结束」那一瞬, 所以这里只需要收尾保存设置。
    //   live_tune 哨兵的撤销由 Apotheosis.cpp 的 aboutToQuit 统一负责。
    saveSettings();
}

void AutoTunePage::buildUi()
{
    // ★ 整页可滚动: 这一页有 4 个分组 + 状态 + 表格 + 详情, 在小窗口(程序默认
    //   960x640)里一定会超出。不套 QScrollArea 的话下半部分会被直接裁掉、
    //   用户根本看不到历史表格 —— 表现为"这页只有设置, 没有数据"。
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);

    auto* content = new QWidget;
    auto* root = new QVBoxLayout(content);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(8);
    scroll->setWidget(content);

    // ── 安全提示 ─────────────────────────────────────────────────────────
    {
        auto* warn = new QLabel(this);
        warn->setWordWrap(true);
        warn->setText(fromUtf8(
            u8"⚠ 本页会【真的修改你正在使用的参数】。请只在靶场里开启, 调好后立刻关闭再实战。\n"
            u8"参数每次改动幅度被限制在 ±20% 以内(beta 上调仅 ±10%), 出现发散或自持抖动会自动回滚, "
            u8"并可随时点「恢复开启前的参数」。"));
        warn->setStyleSheet(QStringLiteral(
            "QLabel { background:#4a3a12; color:#ffd479; padding:8px; border-radius:6px; }"));
        root->addWidget(warn);
    }

    // ── LLM 配置 ─────────────────────────────────────────────────────────
    {
        auto* box = new QGroupBox(fromUtf8(u8"大模型接口 (OpenAI 兼容)"), this);
        auto* form = new QFormLayout(box);

        m_baseUrl = new QLineEdit(box);
        m_baseUrl->setPlaceholderText(fromUtf8(u8"例: https://api.deepseek.com"));
        form->addRow(fromUtf8(u8"接口地址"), m_baseUrl);

        m_apiKey = new QLineEdit(box);
        m_apiKey->setEchoMode(QLineEdit::Password);
        m_apiKey->setPlaceholderText(fromUtf8(u8"sk-..."));
        form->addRow(fromUtf8(u8"API Key"), m_apiKey);

        m_model = new QLineEdit(box);
        m_model->setPlaceholderText(fromUtf8(u8"例: deepseek-chat"));
        form->addRow(fromUtf8(u8"模型名"), m_model);

        m_timeoutMs = new QSpinBox(box);
        m_timeoutMs->setRange(3000, 120000);
        m_timeoutMs->setSingleStep(1000);
        m_timeoutMs->setSuffix(QStringLiteral(" ms"));
        form->addRow(fromUtf8(u8"请求超时"), m_timeoutMs);

        // ★★ 最大输出 token (2026-09-14 新增) ★★
        //
        //   为什么必须让用户能调: 带思维链的模型(deepseek-reasoner / QwQ /
        //   各种 thinking 变体)会把【思考过程也算进这个预算】。预算不够时
        //   就只剩思考、没有正文 —— 症状正是"测试连接成功, 但显示没 content"。
        //   (测试的提示词极短、不用思考, 所以测试通过; 真调参的提示词长得多。)
        //   默认给 4096, 上限放到 32768 以免又被卡住。
        m_maxTokens = new QSpinBox(box);
        m_maxTokens->setRange(256, 32768);
        m_maxTokens->setSingleStep(512);
        m_maxTokens->setSuffix(fromUtf8(u8" token"));
        m_maxTokens->setToolTip(fromUtf8(
            u8"模型一次最多能输出多少 token。\n"
            u8"带思维链的模型会把「思考」也算进这里, 太小就会只剩思考、没有正文。\n"
            u8"调参用的提示词较长, 建议 4096 以上。"));
        form->addRow(fromUtf8(u8"最大输出"), m_maxTokens);

        // 测试连接: 真正发一次最小请求, 把失败原因说清楚。
        // ★ 不能在 UI 线程上发 —— 网络超时会让窗口假死。见 onTestConnection()。
        {
            auto* row = new QHBoxLayout();
            m_testBtn = new QPushButton(fromUtf8(u8"测试连接"), box);
            connect(m_testBtn, &QPushButton::clicked, this, &AutoTunePage::onTestConnection);
            row->addWidget(m_testBtn);

            m_testResult = new QLabel(box);
            m_testResult->setWordWrap(true);
            m_testResult->setTextInteractionFlags(Qt::TextSelectableByMouse);
            row->addWidget(m_testResult, 1);
            form->addRow(row);
        }

        auto* hint = new QLabel(box);
        hint->setWordWrap(true);
        hint->setText(fromUtf8(
            u8"只要兼容 /v1/chat/completions 即可: DeepSeek、Kimi、通义、本地 ollama 都行。"
            u8"本地 ollama 把地址填成 http://127.0.0.1:11434 即可(Key 留空)。"));
        hint->setStyleSheet(QStringLiteral("QLabel { color:#9aa0a6; }"));
        form->addRow(hint);

        root->addWidget(box);
    }

    // ── 尺度基准 ─────────────────────────────────────────────────────────
    {
        auto* box = new QGroupBox(fromUtf8(u8"距离尺度基准"), this);
        auto* v = new QVBoxLayout(box);

        auto* hint = new QLabel(box);
        hint->setWordWrap(true);
        hint->setText(fromUtf8(
            u8"尺度调度会按「当前框高 ÷ 基准框高」调整等效增益 —— 离得近就快、离得远就慢。\n"
            u8"基准的含义是【你的参数是在多远的距离上调出来的】，框高等于它时倍率 = 1.0，"
            u8"与你调参时的行为完全一致。\n"
            u8"★ 你在靶场里看不到检测框的像素高度，所以不用你填：站到你平时交战的"
            u8"典型距离上对着目标停一两秒，按下面这个按钮就行。\n"
            u8"★ 与自动调参开关【无关】—— 不开 agent 也能用。"));
        v->addWidget(hint);

        auto* row = new QHBoxLayout();
        m_setBaseline = new QPushButton(fromUtf8(u8"以当前距离设为基准"), box);
        connect(m_setBaseline, &QPushButton::clicked,
                this, &AutoTunePage::onSetBaseline);
        row->addWidget(m_setBaseline);

        m_baselineInfo = new QLabel(box);
        m_baselineInfo->setTextInteractionFlags(Qt::TextSelectableByMouse);
        row->addWidget(m_baselineInfo, 1);
        v->addLayout(row);

        root->addWidget(box);
    }

    // ── 模式 + 开始/结束 ─────────────────────────────────────────────────
    //
    // ★ 2026-09-14 改版: 从"一个开关一直连调"改成"开始 / 结束 + 模式选择",
    //   每次只调一轮。理由见下面对用户的说明。
    {
        auto* box = new QGroupBox(fromUtf8(u8"调参模式"), this);
        auto* v = new QVBoxLayout(box);

        auto* hint = new QLabel(box);
        hint->setWordWrap(true);
        hint->setText(fromUtf8(
            u8"★ 一次只调一轮。用法: 点【开始采样】→ 你去打 → 点【结束并调一轮】。\n"
            u8"   程序取的是【这两次点击之间】的全部真实日志, 不看别的信号 —— 所以你\n"
            u8"   中途松手、换目标、甩枪都不影响, 采到多少算多少。\n"
            u8"★ 然后你自己去打几枪, 用体感判断这轮是变好还是变坏, 再决定下一轮。\n"
            u8"   程序【不会】自动连调 —— 连调十轮你分不清是哪一轮起的作用, 而且你\n"
            u8"   适应新参数的过程本身就会污染下一轮的数据。"));
        hint->setStyleSheet(QStringLiteral("QLabel { color:#9aa0a6; }"));
        v->addWidget(hint);

        auto* row = new QHBoxLayout();
        row->addWidget(new QLabel(fromUtf8(u8"模式："), box));

        m_mode = new QComboBox(box);
        m_mode->addItem(fromUtf8(u8"1. 静态目标调参"));
        m_mode->addItem(fromUtf8(u8"2. 动态目标调参"));
        m_mode->addItem(fromUtf8(u8"3. 体感调参"));
        m_mode->setToolTip(fromUtf8(
            u8"静态：靶子不动，目标是快速到位 + 停住不抖。主要看收敛、过冲、抖动。\n"
            u8"动态：去追一个移动的靶，目标是贴得住 + 机动后立刻再咬上。\n"
            u8"体感：你在下面用文字说感受，模型结合日志数字一起判断。"));
        connect(m_mode, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &AutoTunePage::onModeChanged);
        row->addWidget(m_mode, 1);
        v->addLayout(row);

        // ── 本轮改动幅度 (2026-09-15) ──────────────────────────────────────
        //
        // ★ 为什么要有这个: 用户实测"显示已应用但体感没什么变化"。查下来是
        //   改动太小 —— 三轮加起来 Kp 只动了 10%、Ki 一动没动。原因是提示词里
        //   写着"幅度要克制"+ 硬夹取只给 ±20%, 两处叠加导致模型永远只挪一点点。
        //   现在把"改多大"交给用户决定, 并且【提示词和硬夹取用同一个数字】。
        //
        // ★ 默认「平稳」= 原来的 ±20%, 所以不选也和老行为一致。
        {
            auto* row2 = new QHBoxLayout();
            row2->addWidget(new QLabel(fromUtf8(u8"改动幅度："), box));

            m_stepStyle = new QComboBox(box);
            m_stepStyle->addItem(fromUtf8(u8"1. 保守（每轮最多 ±10%）"));
            m_stepStyle->addItem(fromUtf8(u8"2. 平稳（每轮最多 ±20%）"));
            m_stepStyle->addItem(fromUtf8(u8"3. 激进（每轮最多 ±50%）"));
            m_stepStyle->setToolTip(fromUtf8(
                u8"这一轮允许 AI 把参数改多少。\n"
                u8"保守：数据不太可信、或只想微调时用。\n"
                u8"平稳：默认。指标明确时可以用满 ±20%。\n"
                u8"激进：当前参数明显不对、想一次调到位时用。\n\n"
                u8"★ 这只是【单轮上限】。选激进不会让 AI 乱改 —— 它仍然要按指标\n"
                u8"   给方向，只是不再畏手畏脚。绝对值域的安全夹取始终生效。\n"
                u8"★ beta（在途补偿）的三档上限都更小：5% / 10% / 20%，因为它补过头\n"
                u8"   会正反馈发散，性质与 Kp/Ki 不同。"));
            connect(m_stepStyle, QOverload<int>::of(&QComboBox::currentIndexChanged),
                    this, &AutoTunePage::onStepStyleChanged);
            row2->addWidget(m_stepStyle, 1);
            v->addLayout(row2);

            m_stepHint = new QLabel(box);
            m_stepHint->setWordWrap(true);
            m_stepHint->setStyleSheet(QStringLiteral("QLabel { color:#9aa0a6; }"));
            v->addWidget(m_stepHint);
        }

        // 模式说明: 随选择变化, 告诉用户"这个模式下该怎么做"
        m_modeHint = new QLabel(box);
        m_modeHint->setWordWrap(true);
        v->addWidget(m_modeHint);

        // 体感模式的输入框 —— 只在模式 3 下显示
        m_feelLabel = new QLabel(fromUtf8(u8"你的感受（必填）："), box);
        v->addWidget(m_feelLabel);
        m_feelText = new QPlainTextEdit(box);
        m_feelText->setPlaceholderText(fromUtf8(
            u8"例如：甩过去总是冲过头一点点 / 跟枪一直拖在后面 / 贴到靶子上会左右抽 / "
            u8"停下之后还在轻微抖"));
        m_feelText->setMaximumHeight(80);
        v->addWidget(m_feelText);

        // 开始 / 结束
        auto* brow = new QHBoxLayout();
        m_begin = new QPushButton(fromUtf8(u8"▶ 开始采样"), box);
        m_begin->setStyleSheet(QStringLiteral(
            "QPushButton { font-weight:bold; padding:6px 16px; }"));
        connect(m_begin, &QPushButton::clicked, this, &AutoTunePage::onBeginSession);
        brow->addWidget(m_begin);

        m_end = new QPushButton(fromUtf8(u8"■ 结束并调一轮"), box);
        m_end->setStyleSheet(QStringLiteral(
            "QPushButton { font-weight:bold; padding:6px 16px; }"));
        connect(m_end, &QPushButton::clicked, this, &AutoTunePage::onEndSession);
        brow->addWidget(m_end);

        m_restore = new QPushButton(fromUtf8(u8"恢复最初参数"), box);
        connect(m_restore, &QPushButton::clicked, this, &AutoTunePage::onRestore);
        brow->addWidget(m_restore);

        brow->addStretch();
        v->addLayout(brow);

        root->addWidget(box);
    }

    // ── 状态 ─────────────────────────────────────────────────────────────
    {
        m_status = new QLabel(this);
        m_status->setWordWrap(true);
        root->addWidget(m_status);

        m_currentParams = new QLabel(this);
        m_currentParams->setWordWrap(true);
        m_currentParams->setStyleSheet(QStringLiteral(
            "QLabel { font-family:Consolas,monospace; background:#1e1f22; "
            "color:#d0d0d0; padding:6px; border-radius:4px; }"));
        root->addWidget(m_currentParams);
    }

    // ── 历史 ─────────────────────────────────────────────────────────────
    {
        m_history = new QTableWidget(0, 7, this);
        m_history->setHorizontalHeaderLabels({
            fromUtf8(u8"轮"), fromUtf8(u8"模式"), fromUtf8(u8"误差中位"),
            fromUtf8(u8"p90"), fromUtf8(u8"过冲Y"), fromUtf8(u8"翻号率"),
            fromUtf8(u8"结果") });

        // ★ 列宽: 不能用 setStretchLastSection(true)。
        //   那会让【最后一列吃掉所有剩余宽度】, 于是"结果"列很宽、而前面 6 列
        //   被压到刚好放得下表头字, 数字挤成一团 —— 实测就是这样。
        //
        //   ★★ 但只把 stretchLastSection 关掉【不够】: ResizeToContents 给的是
        //   "装得下内容的最小宽度", 它不包含表头的需要, 也没有下限保护, 所以
        //   60 年代初的列仍然被压到 16~52px(离屏实测: 轮=16, 时刻=28)。
        //   真正稳的做法是自己算: 前 6 列用 Interactive + 显式设一个够用的
        //   初始宽度, 最后一列才 Stretch。这样列宽不随内容跳、也不会被压扁。
        auto* hh = m_history->horizontalHeader();
        hh->setStretchLastSection(false);
        hh->setSectionResizeMode(0, QHeaderView::Interactive);   // 轮
        hh->setSectionResizeMode(1, QHeaderView::Interactive);   // 时刻
        hh->setSectionResizeMode(2, QHeaderView::Interactive);   // 误差中位
        hh->setSectionResizeMode(3, QHeaderView::Interactive);   // p90
        hh->setSectionResizeMode(4, QHeaderView::Interactive);   // 过冲Y
        hh->setSectionResizeMode(5, QHeaderView::Interactive);   // 翻号率
        hh->setSectionResizeMode(6, QHeaderView::Stretch);       // 结果(最长, 吃剩余)

        // 初始宽度: 按表头文字 + 预期内容给, 宁愿略宽也不要挤。
        // 数字列配等宽字体, 宽度才稳定。
        m_history->setColumnWidth(0, 44);    // 轮: 最多三位数
        m_history->setColumnWidth(1, 64);    // 模式: 静态/动态/体感
        m_history->setColumnWidth(2, 76);    // 误差中位 0.00
        m_history->setColumnWidth(3, 64);    // p90
        m_history->setColumnWidth(4, 84);    // 过冲Y + 单位
        m_history->setColumnWidth(5, 72);    // 翻号率 %
        // ★ 允许用户拖列宽, 但不允许把列拖成 0(那等于把数据藏起来)
        hh->setMinimumSectionSize(40);
        // 数字列用等宽字体, 否则每行宽度不一样、看起来在跳。
        hh->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);

        m_history->verticalHeader()->setVisible(false);
        m_history->verticalHeader()->setDefaultSectionSize(22);
        m_history->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_history->setSelectionMode(QAbstractItemView::SingleSelection);
        m_history->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_history->setShowGrid(false);
        m_history->setAlternatingRowColors(true);
        // 表格自身滚动, 不给它固定最大高度 —— 用 stretch 让它占满剩余空间,
        // 否则窗口拉大时表格不变、下面详情框变得很高, 比例很难看。
        m_history->setMinimumHeight(120);
        connect(m_history, &QTableWidget::cellClicked, this,
                [this](int row, int) {
                    const auto hist =
                        boss::autotune::Runtime::instance().tuner().history();
                    if (row < 0 || static_cast<std::size_t>(row) >= hist.size()) return;
                    const auto& r = hist[static_cast<std::size_t>(row)];
                    QString text;
                    // 这一段数据是怎么来的 —— 事后翻账时最要紧的上下文
                    {
                        const char* mn = boss::autotune::mode_name(r.mode);
                        text += fromUtf8(u8"── 这一轮 ──\n");
                        text += fromUtf8(u8"模式: ") + fromUtf8(mn)
                              + fromUtf8(u8"   改动幅度: ")
                              + fromUtf8(boss::autotune::step_style_name(r.style))
                              + QLatin1Char('\n');
                        if (!r.segment.samples.empty() || r.segment.duration() > 0.0)
                            text += fromUtf8(u8"数据: 采样 %1 秒 / %2 条样本 / 覆盖率 %3%%\n")
                                .arg(r.segment.duration(), 0, 'f', 1)
                                .arg(r.segment.samples.size())
                                .arg(r.segment.coverage * 100.0, 0, 'f', 0);
                        text += fromUtf8(u8"质量: 野值跳变 %1 次 / 框丢失 %2 次 / 框高中位 %3px\n")
                            .arg(r.segment.jumps).arg(r.segment.gaps)
                            .arg(r.segment.bbox_h_median, 0, 'f', 1);
                        text += QLatin1Char('\n');
                    }
                    if (!r.feel_text.empty())
                        text += fromUtf8(u8"── 你的描述 ──\n")
                              + QString::fromStdString(r.feel_text) + QStringLiteral("\n\n");
                    text += fromUtf8(u8"── 实测指标 ──\n")
                          + QString::fromStdString(r.metrics.summary()) + QLatin1Char('\n');
                    text += fromUtf8(u8"\n── 模型想要 ──\n")
                          + QString::fromStdString(r.requested.describe()) + QLatin1Char('\n');
                    text += fromUtf8(u8"\n── 实际生效 ──\n")
                          + QString::fromStdString(r.applied.describe()) + QLatin1Char('\n');
                    text += fromUtf8(u8"\n── 夹取 ──\n")
                          + QString::fromStdString(r.clamp_notes) + QLatin1Char('\n');
                    if (r.rolled_back)
                        text += fromUtf8(u8"\n★ 已回滚: ")
                              + QString::fromStdString(r.rollback_reason) + QLatin1Char('\n');
                    if (!r.error.empty())
                        text += fromUtf8(u8"\n未出结果: ")
                              + QString::fromStdString(r.error) + QLatin1Char('\n');
                    if (!r.model_reply.empty())
                        text += fromUtf8(u8"\n── 模型原话 ──\n")
                              + QString::fromStdString(r.model_reply) + QLatin1Char('\n');
                    m_detail->setPlainText(text);
                });
        root->addWidget(m_history, 3);   // 表格占 3 份

        m_detail = new QPlainTextEdit(this);
        m_detail->setReadOnly(true);
        m_detail->setPlaceholderText(
            fromUtf8(u8"点上面任意一轮, 这里显示那一轮的完整细节(含夹取与回滚原因)。"));
        m_detail->setStyleSheet(QStringLiteral(
            "QPlainTextEdit { font-family:Consolas,monospace; }"));
        m_detail->setMinimumHeight(100);
        root->addWidget(m_detail, 2);    // 详情占 2 份
    }
}

void AutoTunePage::loadSettings()
{
    QSettings s;
    s.beginGroup(QStringLiteral("auto_tune"));
    m_baseUrl->setText(s.value(QStringLiteral("base_url")).toString());
    m_apiKey->setText(s.value(QStringLiteral("api_key")).toString());
    m_model->setText(s.value(QStringLiteral("model")).toString());
    m_timeoutMs->setValue(s.value(QStringLiteral("timeout_ms"), 60000).toInt());
    // ★ 默认 4096: 推理模型的思考也占这个预算, 太小会只剩思考没有正文。
    m_maxTokens->setValue(s.value(QStringLiteral("max_tokens"), 4096).toInt());
    s.endGroup();

    // 同步到 runtime
    pushLlmConfig();

    // 模式与感受也记住 —— 用户会反复用同一个模式, 不该每次重选。
    const int mode = s.value(QStringLiteral("auto_tune/mode"), 1).toInt();
    m_mode->setCurrentIndex(mode < 0 ? 0 : (mode > 2 ? 2 : mode));
    m_feelText->setPlainText(
        s.value(QStringLiteral("auto_tune/feel_text")).toString());
    onModeChanged(m_mode->currentIndex());

    // 改动幅度也记住 —— 用户会反复用同一档。
    // ★ 默认 1(平稳) = 原来的 ±20%, 所以没存过键的老用户行为不变。
    {
        const int style = s.value(QStringLiteral("auto_tune/step_style"), 1).toInt();
        m_stepStyle->setCurrentIndex(style < 0 ? 0 : (style > 2 ? 2 : style));
        onStepStyleChanged(m_stepStyle->currentIndex());
    }

    // 任何一个接口字段变了就立刻同步 + 落盘。
    // ★ 集中调 pushLlmConfig(), 不再每个控件写一遍(那样加字段容易漏一处)。
    const auto onLlmChanged = [this]() { pushLlmConfig(); saveSettings(); };
    connect(m_baseUrl, &QLineEdit::textChanged, this, onLlmChanged);
    connect(m_apiKey, &QLineEdit::textChanged, this, onLlmChanged);
    connect(m_model, &QLineEdit::textChanged, this, onLlmChanged);
    connect(m_timeoutMs, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [onLlmChanged](int) { onLlmChanged(); });
    connect(m_maxTokens, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [onLlmChanged](int) { onLlmChanged(); });
}

void AutoTunePage::saveSettings()
{
    QSettings s;
    s.beginGroup(QStringLiteral("auto_tune"));
    s.setValue(QStringLiteral("base_url"), m_baseUrl->text());
    s.setValue(QStringLiteral("api_key"), m_apiKey->text());
    s.setValue(QStringLiteral("model"), m_model->text());
    s.setValue(QStringLiteral("timeout_ms"), m_timeoutMs->value());
    s.setValue(QStringLiteral("max_tokens"), m_maxTokens->value());
    // ★ 模式与感受也存 —— 采样到一半关程序, 下次打开还认得上次的选择。
    s.setValue(QStringLiteral("mode"), m_mode->currentIndex());
    s.setValue(QStringLiteral("feel_text"), m_feelText->toPlainText());
    s.endGroup();
}

// ── 模式切换 ────────────────────────────────────────────────────────────────
//
// ★ 模式不是给模型看的标签, 它真的改变两件事:
//   ① 给模型的解读提示(静态看收敛, 动态看跟随滞后, 体感以用户原话为主)
//   ② judge_segment 的干净度判据(静态模式下尺寸变化大 = 数据选错模式了)
void AutoTunePage::onModeChanged(int idx)
{
    auto& rt = boss::autotune::Runtime::instance();
    rt.set_mode(idx);

    const bool feel = (idx == 2);
    m_feelLabel->setVisible(feel);
    m_feelText->setVisible(feel);

    if (idx == 0)
        m_modeHint->setText(fromUtf8(
            u8"【静态目标】站桩靶、不动的人。目标：甩过去快速到位、停住不抖。\n"
            u8"点「开始」后往靶子上甩几次、再停住不动, 让它自己收敛。"));
    else if (idx == 1)
        m_modeHint->setText(fromUtf8(
            u8"【动态目标】点「开始」后去追一个横移/走位的目标。目标：贴得住、"
            u8"它急停或反向之后能立刻再咬上。\n"
            u8"靶子怎么动没有固定要求 —— agent 会从日志里自己分析这段运动, "
            u8"你按自己平时实战的方式追就行。"));
    else
        m_modeHint->setText(fromUtf8(
            u8"【体感调参】你在下面写一句感受, agent 结合这段日志的数字一起判断。\n"
            u8"★ 你的话是主证据, 数字是佐证 —— 两者冲突时以你的感受为准。"));

    // ★ 切换模式时若正在采样, 直接取消 —— 免得一半数据按旧模式判、一半按新的。
    if (rt.session_open()) onEndSession();
    else syncSessionControls();
}

// ── 本轮改动幅度 ────────────────────────────────────────────────────────────
void AutoTunePage::onStepStyleChanged(int idx)
{
    auto& rt = boss::autotune::Runtime::instance();
    rt.set_step_style(idx);

    if (m_stepHint)
    {
        if (idx == 0)
            m_stepHint->setText(fromUtf8(
                u8"保守：每轮最多 ±10%。数据不太可信、或只想微调时用。\n"
                u8"如果指标看着已经不错, AI 会把参数原样返回(这很正常, 不是出错)。"));
        else if (idx == 2)
            m_stepHint->setText(fromUtf8(
                u8"激进：每轮最多 ±50%。当前参数明显不对、想一次调到位时用。\n"
                u8"★ 选它 AI 才敢一次大幅调整。绝对值域的安全夹取仍然生效,\n"
                u8"   beta 的上调仍限制在 +20%(它补过头会发散)。"));
        else
            m_stepHint->setText(fromUtf8(
                u8"平稳（默认）：每轮最多 ±20%, 与原行为一致。"));
    }

    QSettings s;
    s.setValue(QStringLiteral("auto_tune/step_style"), idx);
}

void AutoTunePage::syncSessionControls()
{
    auto& rt = boss::autotune::Runtime::instance();
    const bool open = rt.session_open();
    // 采样中: 不能改模式(会污染判据), 也不能重复点开始
    m_begin->setEnabled(!open);
    m_end->setEnabled(open);
    m_mode->setEnabled(!open);
    // ★ 改动幅度【不】跟着禁用 —— 它不影响数据判据, 只影响这一轮改多大,
    //   用户采样期间想起来要改也应当允许。
    m_feelText->setReadOnly(open);
    m_restore->setEnabled(!open && rt.has_initial());
    m_begin->setText(open ? fromUtf8(u8"● 采样中…") : fromUtf8(u8"▶ 开始采样"));
}

// ── 「开始」─────────────────────────────────────────────────────────────────
void AutoTunePage::onBeginSession()
{
    auto& rt = boss::autotune::Runtime::instance();
    if (!rt.installed())
    {
        QMessageBox::warning(this, fromUtf8(u8"未接入"),
            fromUtf8(u8"调参功能尚未接入运行时(内部错误), 无法开始。"));
        return;
    }
    if (m_baseUrl->text().trimmed().isEmpty())
    {
        QMessageBox::warning(this, fromUtf8(u8"缺少配置"),
            fromUtf8(u8"请先在上面填写接口地址与模型名。"));
        return;
    }
    const int mode = m_mode->currentIndex();
    if (mode == 2 && m_feelText->toPlainText().trimmed().isEmpty())
    {
        QMessageBox::warning(this, fromUtf8(u8"体感模式需要描述"),
            fromUtf8(u8"体感调参是根据你的感受来调的。\n\n"
                       u8"请先在「你的感受」里写一句, 比如：\n"
                       u8"  · 跟枪一直拖在后面\n"
                       u8"  · 甩过去总是冲过头\n"
                       u8"  · 贴到靶子上会左右抽"));
        return;
    }

    rt.set_mode(mode);
    rt.set_feel_text(m_feelText->toPlainText().toStdString());
    rt.begin_session();
    syncSessionControls();

    QMessageBox::information(this, fromUtf8(u8"开始采样"),
        fromUtf8(u8"已经开始记录了。现在去靶场正常打 —— 甩枪、跟枪、换目标都随意。\n\n"
                   u8"· 建议持续 3~5 秒以上(至少 1 秒), 太短算不出可信的分位数。\n"
                   u8"· 打完了不用先松手, 直接点「结束并调一轮」就行。\n"
                   u8"· 采到的数据条数会实时显示在上面的状态栏里。"));
}

// ── 「结束」─────────────────────────────────────────────────────────────────
//
// ★ 这里会阻塞到网络请求返回。这是有意的: 用户刚按完「结束」, 本来就在等
//   结果, 而一轮只发一次请求。给鼠标转个忙碌光标, 免得看起来像卡死。
void AutoTunePage::onEndSession()
{
    auto& rt = boss::autotune::Runtime::instance();
    if (!rt.session_open()) return;

    // 采样中的模式/感受到此定稿
    rt.set_mode(m_mode->currentIndex());
    rt.set_feel_text(m_feelText->toPlainText().toStdString());

    QApplication::setOverrideCursor(Qt::WaitCursor);
    rt.end_session();
    QApplication::restoreOverrideCursor();

    syncSessionControls();
    onRefresh();

    // ★★ 这一轮到底成没成 —— 必须当场说清楚, 不能只往表格里塞一行。 ★★
    //
    //   为什么值得弹窗: 原来的失败形态是【静默的】—— 参数没生效, 界面上却
    //   写着"已应用", 用户以为调了、体感却没变, 只能得出"这功能不好使"的
    //   结论(实测反馈正是如此)。失败必须比成功更响。
    {
        const auto hist = rt.tuner().history();
        if (!hist.empty())
        {
            const auto& last = hist.back();
            const QString detail = QString::fromStdString(last.error);
            // 只对"最近这一轮"提示: 用轮次号判断, 避免重复弹。
            if (last.index != m_lastNotifiedRound)
            {
                m_lastNotifiedRound = last.index;
                if (!last.error.empty())
                {
                    QMessageBox::warning(this, fromUtf8(u8"这一轮没有生效"),
                        fromUtf8(u8"参数【没有】真正应用到运行中的控制器:\n\n")
                        + detail
                        + fromUtf8(u8"\n\n参数仍是上一轮的值, 不会影响你现在的设置。"));
                }
                else if (last.rolled_back)
                {
                    QMessageBox::information(this, fromUtf8(u8"已自动回滚"),
                        fromUtf8(u8"这一轮的数据显示参数变差了, 已退回上一组已知可用的值。\n\n")
                        + fromUtf8(u8"原因: ") + QString::fromStdString(last.rollback_reason));
                }
            }
        }
    }
}

// 以当前距离设为基准: 取最近几秒框高的中位数存下来。
//
// ★ 为什么用中位数: 遮挡/出画会产生离群的小框, 均值会被拉偏。
// ★ 为什么要"停一两秒": 缓冲里是最近约 7 秒的框高。如果你一边快速拉近拉远
//   一边按, 中位数反映的是那段移动过程, 不是你想当标准的那个距离。
// ★ 与调参会话无关 —— 这是它存在的意义(不调参也能设基准)。
void AutoTunePage::onSetBaseline()
{
    auto& rt = boss::autotune::Runtime::instance();
    const int have = rt.recent_height_samples();

    if (!rt.set_baseline_from_recent())
    {
        QMessageBox::information(this, fromUtf8(u8"数据不足"),
            fromUtf8(u8"最近没有足够的检测数据（需要 30 条以上）。\n\n"
                       u8"请先启动识别、锁住一个目标、对着它停一两秒，再按这个按钮。"));
        return;
    }

    const double h = rt.learned_baseline();
    QMessageBox::information(this, fromUtf8(u8"已设为基准"),
        fromUtf8(u8"基准框高 = %1 px（取自最近 %2 条检测的框高中位数）。\n\n"
                   u8"之后框高比它大（更近）就加快，比它小（更远）就减慢；"
                   u8"框高正好等于它时与你现在的行为完全一致。")
            .arg(h, 0, 'f', 1).arg(have));
}

// 测试 AI 连接。
//
// ★ 为什么不只是一个"通/不通":
//   失败原因决定的下一步动作完全不同 ——
//     地址写错      -> 改地址 (常见: 多写了 /v1, 或少写了 https://)
//     Key 无效      -> 换 Key
//     模型名不存在  -> 换模型名 (地址和 Key 都是对的)
//     连接超时      -> 查网络 / 本地服务没起
//   所以这里把【HTTP 状态码 + 服务端返回的 message】原样显示出来。
//
// ★ 为什么必须放到后台线程:
//   chat_completion 是阻塞的, 超时可达 120 秒。在 UI 线程上调用会让整个窗口
//   卡死, 用户会以为程序崩了。这里用 QtConcurrent 起一个后台任务, 完成后
//   回到 UI 线程显示结果。测试期间禁用按钮, 防止连点堆出一堆请求。
void AutoTunePage::onTestConnection()
{
    pushLlmConfig();
    auto cfg = boss::autotune::Runtime::instance().llm();

    if (cfg.base_url.empty())
    {
        m_testResult->setStyleSheet(QStringLiteral("QLabel { color:#ff8a80; }"));
        m_testResult->setText(fromUtf8(u8"✗ 请先填写接口地址"));
        return;
    }
    if (cfg.model.empty())
    {
        m_testResult->setStyleSheet(QStringLiteral("QLabel { color:#ff8a80; }"));
        m_testResult->setText(fromUtf8(u8"✗ 请先填写模型名"));
        return;
    }

    m_testBtn->setEnabled(false);
    m_testResult->setStyleSheet(QStringLiteral("QLabel { color:#9aa0a6; }"));
    m_testResult->setText(fromUtf8(u8"正在测试…"));

    // 用一个最小请求: 让它只回一个 JSON, 验证"地址/Key/模型名"三件事都对。
    const std::string sys = u8"你是一个测试助手。";
    const std::string usr = u8"请只回复一个 JSON: {\"ok\":true}";

    // ★ 用 std::thread + QMetaObject::invokeMethod 而不是 QtConcurrent:
    //   项目当前只 find_package(Qt6 ... Widgets), 引 QtConcurrent 还要改
    //   CMake 的组件列表。这里只需要"后台跑一个阻塞调用, 完成后回 UI 线程",
    //   标准线程 + 一次队列调用就够了, 不多一个依赖。
    auto* worker = new std::thread([this, cfg, sys, usr]() {
        const auto r = boss::autotune::chat_completion(cfg, sys, usr);
        // 回到 UI 线程再碰控件。lambda 里捕获的是 this, 而本页在窗口关闭时
        // 才销毁 —— 测试期间窗口不会消失, 所以这里不需要额外的生命周期守卫。
        QMetaObject::invokeMethod(this, [this, r, cfg]() {
            m_testBtn->setEnabled(true);

            if (r.ok)
            {
                m_testResult->setStyleSheet(QStringLiteral("QLabel { color:#81c784; }"));
                QString msg = fromUtf8(u8"✓ 连接正常 (")
                    + QString::number(static_cast<int>(r.elapsed_ms)) + fromUtf8(u8" ms)");
                // 顺带检查它到底听没听懂"只回 JSON" —— 这直接影响调参能不能解析
                const QString js = QString::fromStdString(
                    boss::autotune::extract_json_object(r.text));
                if (js.isEmpty())
                    msg += fromUtf8(u8"\n⚠ 但回答里没有 JSON, 调参时可能解析失败。");

                // ★★ 即使这里通过了, 也要提醒"输出预算快用光"这件事。 ★★
                //   测试用的提示词极短, 所以【测试通过不代表真调参能通过】:
                //   真调参的提示词长得多, 推理模型会思考更久, 同一个 max_tokens
                //   就可能在那边被吃光 —— 那正是"测试连接成功, 但显示没 content"
                //   这个症状的来源。用量超过八成就在这儿提前警告。
                if (r.completion_tokens > 0 &&
                    r.completion_tokens * 10 >= cfg.max_tokens * 8)
                {
                    msg += fromUtf8(u8"\n⚠ 本次已用 ")
                         + QString::number(r.completion_tokens)
                         + fromUtf8(u8" / ") + QString::number(cfg.max_tokens)
                         + fromUtf8(u8" token。真调参的提示词更长, 很可能不够 ——"
                                    u8"建议把「最大输出」调到 4096 以上。");
                }
                m_testResult->setText(msg);
                return;
            }

            m_testResult->setStyleSheet(QStringLiteral("QLabel { color:#ff8a80; }"));
            QString msg = fromUtf8(u8"✗ ") + QString::fromStdString(r.error);

            // 按状态码给出"下一步该改什么"
            if (r.http_status == 401 || r.http_status == 403)
                msg += fromUtf8(u8"\n→ API Key 无效或无权限");
            else if (r.http_status == 404)
                msg += fromUtf8(u8"\n→ 地址或模型名不对。检查接口地址是否多写了 /v1");
            else if (r.http_status == 429)
                msg += fromUtf8(u8"\n→ 触发限流, 稍后再试");
            else if (r.http_status >= 500)
                msg += fromUtf8(u8"\n→ 服务端错误, 稍后再试");
            else if (r.http_status == 0)
                msg += fromUtf8(u8"\n→ 连不上。检查网络、地址拼写; 本地服务请确认已启动");

            m_testResult->setText(msg);
        }, Qt::QueuedConnection);
    });
    worker->detach();
    delete worker;   // detach 之后句柄本身可以立刻释放
}

// 把界面上填的接口配置推给 runtime (原来散在 loadSettings 的各个 connect 里)
void AutoTunePage::pushLlmConfig()
{
    boss::autotune::LlmConfig cfg;
    cfg.base_url = m_baseUrl->text().trimmed().toStdString();
    cfg.api_key = m_apiKey->text().trimmed().toStdString();
    cfg.model = m_model->text().trimmed().toStdString();
    cfg.timeout_ms = m_timeoutMs->value();
    // ★ 必须一起推 —— 漏了这个字段, 推理模型就会"只剩思考没有正文"
    //   (思考也占 max_tokens 预算)。
    cfg.max_tokens = m_maxTokens->value();
    boss::autotune::Runtime::instance().set_llm(cfg);
}

void AutoTunePage::onRestore()
{
    auto& rt = boss::autotune::Runtime::instance();
    if (!rt.has_initial())
    {
        QMessageBox::information(this, fromUtf8(u8"无可回退"),
            fromUtf8(u8"还没有记录到开始调参前的参数 —— 请先点一次「开始采样」。"));
        return;
    }
    rt.restore_initial();
    QMessageBox::information(this, fromUtf8(u8"已恢复"),
        fromUtf8(u8"参数已恢复到【第一次开始调参之前】的那一组。\n\n"
                   u8"（注意: 是第一次, 不是上一次 —— 连调几轮之后回退会一路退回原点。）"));
}

void AutoTunePage::appendRound(const boss::autotune::Round& r)
{
    const int row = m_history->rowCount();
    m_history->insertRow(row);

    auto set = [&](int col, const QString& text) {
        m_history->setItem(row, col, new QTableWidgetItem(text));
    };
    set(0, QString::number(r.index));
    // 模式: 事后翻账时最要紧的一列 —— "这条是在哪种靶子上调出来的"
    switch (r.mode)
    {
    case boss::autotune::TuneMode::Static: set(1, fromUtf8(u8"静态")); break;
    case boss::autotune::TuneMode::Moving: set(1, fromUtf8(u8"动态")); break;
    case boss::autotune::TuneMode::Feel:   set(1, fromUtf8(u8"体感")); break;
    default:                               set(1, QStringLiteral("-")); break;
    }
    set(2, QString::number(r.metrics.err_median, 'f', 2));
    set(3, QString::number(r.metrics.err_p90, 'f', 2));
    set(4, QString::number(r.metrics.overshoot_y, 'f', 2));
    set(5, QString::number(r.metrics.flip_ratio * 100.0, 'f', 0) + QStringLiteral("%"));

    QString result;
    if (!r.error.empty())        result = fromUtf8(u8"未出结果: ") + QString::fromStdString(r.error);
    else if (r.rolled_back)      result = fromUtf8(u8"★回滚: ") + QString::fromStdString(r.rollback_reason);
    else                         result = fromUtf8(u8"已应用");
    // 结果列可能很长, 单行显示会被截; 完整内容在下面的详情里。
    set(6, result);
    m_history->item(row, 6)->setToolTip(result);

    // ★ 「已应用」必须配得上这个说法: 真的生效了才让它是绿的。
    //   这一行是给"参数没生效却显示成功"那类事故兜底的 —— 用户扫一眼颜色
    //   就知道这一轮到底有没有落地, 不用去读详情。
    if (!r.error.empty())
        m_history->item(row, 6)->setForeground(QColor(0xff, 0x8a, 0x80));
    else if (r.rolled_back)
        m_history->item(row, 6)->setForeground(QColor(0xff, 0xd4, 0x79));
    else
        m_history->item(row, 6)->setForeground(QColor(0x81, 0xc7, 0x84));

    m_history->scrollToBottom();
}

void AutoTunePage::onRefresh()
{
    auto& rt = boss::autotune::Runtime::instance();

    m_status->setText(QString::fromStdString(rt.status_text()));
    m_currentParams->setText(QString::fromStdString(rt.current_knobs().describe()));
    syncSessionControls();

    // 基准状态: 显示当前值 + 缓冲里有多少条可用数据。
    // ★ 把"攒了多少条"显示出来, 用户才知道按钮为什么点不动(需要 30 条以上)。
    {
        const double base = rt.learned_baseline();
        const int n = rt.recent_height_samples();
        if (base > 0.0)
            m_baselineInfo->setText(
                fromUtf8(u8"当前基准：%1 px　（最近可用数据 %2 条，可直接重设）")
                    .arg(base, 0, 'f', 1).arg(n));
        else if (n >= 30)
            m_baselineInfo->setText(
                fromUtf8(u8"未设置　（已有 %1 条数据，可以按按钮了）").arg(n));
        else
            m_baselineInfo->setText(
                fromUtf8(u8"未设置　（需要 30 条检测数据，现在 %1 条 —— 请先启动识别并锁住目标）")
                    .arg(n));
    }

    const auto hist = rt.tuner().history();
    while (m_shownRounds < static_cast<int>(hist.size()))
    {
        appendRound(hist[static_cast<std::size_t>(m_shownRounds)]);
        ++m_shownRounds;
    }
    if (hist.empty()) m_shownRounds = 0;
}
