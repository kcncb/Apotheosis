// 离屏渲染 AutoTunePage 并测量真实几何。
//
// ★ 为什么要有这个: 列宽问题是【纯视觉】的, 逻辑测试测不到 —— 只能真的把
//   控件建出来、布局一遍、再读 widget 的实际像素宽度。之前在 setStretchLastSection
//   上踩过: 代码看着没问题("让最后一列占满剩余"), 实际效果是前 6 列被压扁。
//
// 用法: autotune_layout_test.exe   (需要 Qt 的 offscreen 平台插件, 不开窗口)
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QHeaderView>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QTimer>
#include <QWidget>
#include <cstdio>

#include "pages/AutoTunePage.h"

// ★ 只为了满足链接: AutoTunePage 会调 uninstall_production_hooks(退出时删
//   live_tune 哨兵)。真实现(autotune_hooks.cpp)会拖进 config.cpp ->
//   Apotheosis.h -> OpenCV, 而本测试只关心【几何】, 完全不需要那条链。
//   这里给个空壳, 顺便把"被调用过"记下来 —— 布局测试本来也不该触发它。
namespace boss { namespace autotune {
void uninstall_production_hooks() {}
} }

static int g_fail = 0;
static void check(bool ok, const char* what)
{
    std::printf("  [%s] %s\n", ok ? " ok " : "FAIL", what);
    if (!ok) ++g_fail;
}

int main(int argc, char** argv)
{
    // offscreen: 不弹窗、不抢焦点, 可以在没有桌面的环境跑
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    std::printf("=== autotune_layout_test: 自动调参页真实几何 ===\n");

    // ★★ 先清掉本进程上次留下的 QSettings ★★
    // 页面构造时会 loadSettings(), 而【析构】时会 saveSettings() —— 本测试后面
    // 又要切到「体感」模式去量感受框, 于是那个 index 被存下来; 下一次运行
    // loadSettings() 读到它, "默认选中动态目标"这条断言就必然变红。也就是说
    // 【这个测试第一次跑是绿的, 之后每次都红】—— 实测踩到过。
    // 判据本身要测的是"没有用户偏好时的默认值", 所以正确做法是先删掉那个键。
    {
        QSettings s;
        s.beginGroup(QStringLiteral("auto_tune"));
        s.remove(QStringLiteral("mode"));
        s.endGroup();
        s.sync();
    }

    AutoTunePage page;
    page.resize(900, 600);          // 接近程序默认窗口的可用区域
    page.show();
    // 让布局跑完(Qt 的布局是延迟的)
    for (int i = 0; i < 3; ++i) { app.processEvents(); }

    // ── 找到表格 ────────────────────────────────────────────────────────
    auto* table = page.findChild<QTableWidget*>();
    check(table != nullptr, "页面里有历史表格");
    if (!table) { std::printf("\n=== %d 项失败 ===\n", g_fail); return 1; }

    auto* hh = table->horizontalHeader();
    check(hh != nullptr, "表格有表头");
    check(table->columnCount() == 7, "表格是 7 列");

    // ★ 核心: stretchLastSection 必须是 false —— 它就是"最后一列吃掉全部
    //   剩余宽度、前面几列被压扁"的成因。
    check(!hh->stretchLastSection(),
          "★ stretchLastSection 已关闭(否则前几列会被压扁)");

    // 真实宽度
    std::printf("\n  列宽实测(表头文字 / 像素宽):\n");
    int total = 0;
    for (int c = 0; c < table->columnCount(); ++c)
    {
        const int w = table->columnWidth(c);
        total += w;
        const QString name = table->horizontalHeaderItem(c)->text();
        std::printf("    [%d] %-10s %4d px\n",
                    c, name.toUtf8().constData(), w);
    }
    std::printf("    合计 %d px (视口 %d px)\n", total, table->viewport()->width());

    // ── 每一列都必须真的能显示下自己的表头 ──────────────────────────────
    // "轮" "时刻" "误差中位" "p90" "过冲Y" "翻号率" "结果"
    // 用最小合理阈值而不是精确值: 字体/DPI 会变, 但"挤到看不见"必须被抓到。
    const int kMin[] = {24, 50, 60, 30, 50, 50, 60};
    for (int c = 0; c < 7; ++c)
    {
        const int w = table->columnWidth(c);
        char buf[128];
        std::snprintf(buf, sizeof(buf),
            "第 %d 列宽度 %d >= %d (表头放得下)", c, w, kMin[c]);
        check(w >= kMin[c], buf);
    }

    // 表格不能被压没高度
    check(table->height() >= 120, "表格有足够高度");
    std::printf("    表格高度 %d px, 详情高度(若有) %d px\n",
                table->height(), 0);

    // ── 整页必须可滚动 ──────────────────────────────────────────────────
    // 没有 QScrollArea 的话, 小窗口下下半部分会被直接裁掉。
    auto* scroll = page.findChild<QScrollArea*>();
    check(scroll != nullptr, "★ 整页包在 QScrollArea 里(小窗口不裁切)");
    if (scroll)
    {
        check(scroll->widgetResizable(), "QScrollArea 自适应宽高");
        // 内容比视口高 -> 说明确实需要滚动(证明这个包裹不是多余的)
        const int contentH = scroll->widget() ? scroll->widget()->sizeHint().height() : 0;
        std::printf("    内容建议高度 %d px, 视口 %d px\n",
                    contentH, scroll->viewport()->height());
    }

    // ── 基准那一组必须存在且按钮可用 ────────────────────────────────────
    bool foundBaselineBtn = false;
    for (auto* b : page.findChildren<QPushButton*>())
        if (b->text().contains(QString::fromUtf8(u8"设为基准")))
            foundBaselineBtn = true;
    check(foundBaselineBtn, "★ 有「以当前距离设为基准」按钮");

    // ── 模式选择 + 开始/结束 (2026-09-14) ───────────────────────────────
    //
    // ★ 这几个控件是这一版的核心: 没有它们, "一次只调一轮"就无从操作。
    //   逻辑测试测不到界面有没有这些按钮, 所以在这一层断言。
    auto* mode = page.findChild<QComboBox*>();
    check(mode != nullptr, "★ 有模式下拉框");
    if (mode)
    {
        check(mode->count() == 3, "★ 模式是 3 个(静态/动态/体感)");
        const QString t0 = mode->itemText(0);
        const QString t1 = mode->itemText(1);
        const QString t2 = mode->itemText(2);
        check(t0.contains(QString::fromUtf8(u8"静态")), "  模式1 = 静态目标");
        check(t1.contains(QString::fromUtf8(u8"动态")), "  模式2 = 动态目标");
        check(t2.contains(QString::fromUtf8(u8"体感")), "  模式3 = 体感");
        // 默认应该是【动态】—— 用户主要在动靶上判断手感(索引 1)
        check(mode->currentIndex() == 1, "  默认选中「动态目标」");
    }

    bool foundBegin = false, foundEnd = false;
    for (auto* b : page.findChildren<QPushButton*>())
    {
        if (b->text().contains(QString::fromUtf8(u8"开始"))) foundBegin = true;
        if (b->text().contains(QString::fromUtf8(u8"结束"))) foundEnd = true;
    }
    check(foundBegin, "★ 有「开始采样」按钮");
    check(foundEnd, "★ 有「结束并调一轮」按钮");

    // ★ 不可以再有"一直连调"的开关 —— 那一版已被明确否决。
    bool foundToggle = false;
    for (auto* c : page.findChildren<QCheckBox*>())
        if (c->text().contains(QString::fromUtf8(u8"自动调参"))) foundToggle = true;
    check(!foundToggle, "★ 没有遗留的「启用自动调参」连调开关");

    // ── ★「最大输出」必须存在且够大 (2026-09-14) ───────────────────────
    //
    // 用户实测报「测试连接成功, 但显示没 content」, 根因是推理模型把
    // max_tokens 全花在思维链上。修法之一是把这个值提到 4096 并让用户能调。
    // ★ 这里断言【界面上真的有这个框、而且范围够大】—— 否则用户遇到
    //   截断时没有任何办法自救, 只能改代码。
    {
        QSpinBox* maxTok = nullptr;
        for (auto* s : page.findChildren<QSpinBox*>())
            if (s->suffix().contains(QString::fromUtf8(u8"token")))
                maxTok = s;
        check(maxTok != nullptr, "★ 有「最大输出」输入框(token 单位)");
        if (maxTok)
        {
            check(maxTok->value() >= 4096,
                  "★ 默认 >= 4096(推理模型的思考也占这个预算)");
            check(maxTok->maximum() >= 32768,
                  "★ 上限够大 —— 推理模型可以调到 32768");
        }
    }

    // ── ★「改动幅度」三档下拉框必须存在 (2026-09-15) ───────────────────
    //
    // ★ 用户实测"显示已应用但体感没什么变化", 根因之一是提示词让模型
    //   "幅度要克制"+ 硬夹取只有 ±20%。修法是把这个决定权交给用户。
    //   ★ 这里断言界面上真的有这个框、而且三档都在 —— 否则用户遇到
    //     "改了没感觉"时没有任何自救手段。
    {
        QComboBox* stepBox = nullptr;
        for (auto* c : page.findChildren<QComboBox*>())
            if (c->count() == 3 && c->itemText(0).contains(QString::fromUtf8(u8"保守")))
                stepBox = c;
        check(stepBox != nullptr, "★ 有「改动幅度」下拉框(三档)");
        if (stepBox)
        {
            check(stepBox->itemText(0).contains(QString::fromUtf8(u8"保守")),
                  "  第 1 档 = 保守");
            check(stepBox->itemText(1).contains(QString::fromUtf8(u8"平稳")),
                  "  第 2 档 = 平稳");
            check(stepBox->itemText(2).contains(QString::fromUtf8(u8"激进")),
                  "  第 3 档 = 激进");
            // ★ 档位文案里必须写出百分比 —— 用户得知道"激进"到底多大
            check(stepBox->itemText(0).contains(QString::fromUtf8(u8"10%")),
                  "★ 保守档写明 ±10%");
            check(stepBox->itemText(2).contains(QString::fromUtf8(u8"50%")),
                  "★ 激进档写明 ±50%");
            check(stepBox->currentIndex() == 1,
                  "★ 默认 = 平稳(与原行为一致)");
        }
    }

    // ── 体感输入框: 只在模式 3 显示 ─────────────────────────────────────
    QPlainTextEdit* feel = nullptr;
    for (auto* e : page.findChildren<QPlainTextEdit*>())
        if (e->placeholderText().contains(QString::fromUtf8(u8"例如")))
            feel = e;
    check(feel != nullptr, "★ 有体感描述输入框");
    if (feel && mode)
    {
        // 切到静态 -> 感受框应当隐藏(它只对体感模式有意义)
        mode->setCurrentIndex(0);
        for (int i = 0; i < 2; ++i) app.processEvents();
        check(feel->isHidden(), "  模式=静态 时, 感受框隐藏");

        mode->setCurrentIndex(2);
        for (int i = 0; i < 2; ++i) app.processEvents();
        check(!feel->isHidden(), "  模式=体感 时, 感受框显示");
    }

    // ── 会话状态联动: 默认(未采样)时「结束」必须点不动 ─────────────────
    // ★ 否则用户会点一个没有数据的「结束」, 得到一句莫名其妙的错误。
    {
        QPushButton* endBtn = nullptr;
        QPushButton* beginBtn = nullptr;
        for (auto* b : page.findChildren<QPushButton*>())
        {
            if (b->text().contains(QString::fromUtf8(u8"结束"))) endBtn = b;
            if (b->text().contains(QString::fromUtf8(u8"开始"))) beginBtn = b;
        }
        if (endBtn && beginBtn)
        {
            check(endBtn->isEnabled() == false, "★ 空闲时「结束」不可点");
            check(beginBtn->isEnabled() == true, "★ 空闲时「开始」可点");
        }
    }

    std::printf("\n=== %d 项失败 ===\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
