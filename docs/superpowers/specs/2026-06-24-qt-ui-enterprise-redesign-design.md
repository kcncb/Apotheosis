# Apotheosis Qt UI 企业级重设计 — 设计规格

- 日期:2026-06-24
- 范围:`qt_ui/`(Qt6 Widgets 前端,独立 CMake)
- 状态:已通过可视化原型确认方向,待用户 review 本规格

## 1. 背景与问题

当前 `qt_ui/` 是一套底子不差的 Linear 风浅色主题(Fusion + `style/theme.qss`),
但有几处一眼泄露"个人项目"感,使它达不到"企业级展示"水平:

1. 强调色不统一:主题用 `#5E6AD2`(紫),但 `MainWindow.cpp` 里"保存配置"按钮
   硬编码成 `#2563EB`(蓝),两个强调色打架。
2. 用 emoji 当图标:`💾 保存配置` / `✓ 已保存`。
3. 导航是上下两层 `QTabBar`,没有应用外壳/品牌区,像"设置对话框"而非"产品"。
4. 缺品牌、缺层次:无 Logo/顶栏,每页都是等密度卡片竖排。
5. 缺"展示级"首屏:没有能体现产品价值的概览/仪表盘。

## 2. 目标

把 `qt_ui/` 升级为**真正的企业级展示型桌面应用**,保持浅色 Linear / 现代 SaaS 气质,
在现有页面内容基础上重做**外壳、骨架、整体基调**,并新增一个**概览仪表盘首屏**。

非目标(本期不做):

- 深色模式(用户已确认本期只做浅色;Token 体系预留,后续可加)。
- 16 个功能页的逐页深度重构(本期统一换皮 + 组件升级,内容沿用)。
- 引入 QtCharts 等新重依赖(用现有自绘 `FpsGraphWidget` 泛化代替)。
- 改动 runtime / detector / capture / config 的任何契约(纯前端外壳与样式)。

## 3. 已确认的视觉语言(经原型验证)

整体 = **顶部导航外壳 + 浅色 Linear + 概览仪表盘 + 精修组件**。

### 3.1 外壳(App Shell)

- **顶部导航栏(TopNav)**:左=品牌(紫色圆角方块 + `ti-crosshair` 标记 + "Apotheosis"
  字标);中=主导航(概览 / 会话 / 配置 / 控制 / 监控),当前项深色文字 + 2px 紫色下划线;
  右=全局操作(实时会话状态药丸、`保存配置` 主按钮、账户头像)。
- **二级导航侧边栏(SideNav)**:当前主分组有子页时,在 TopNav 下方、内容区左侧显示一条竖直
  侧边栏,列出该分组的子页(图标 + 名称),顶部带分组名;选中项紫色高亮(`#EEF0FC` 底 + 紫字 +
  紫图标)。`概览` 无子页,则隐藏侧边栏,仪表盘占满宽度。
  (早期试过"顶栏下方一排分段控件"方案,确认观感偏弱,已改为本侧边栏。)
- 去掉 `MainWindow` 现有的两层 `QTabBar` 与底部 emoji 保存按钮。
- 内容区浅灰底(`#F6F7F9`),卡片白底带 1px 描边 + 极轻阴影。

### 3.2 概览仪表盘首屏(OverviewPage,新增)

作为 `概览` 主分组的落地页,复用既有遥测配线,curated 成"产品的脸":

- **状态英雄区(Hero)**:运行状态(绿点 + "推理运行中" / 灰点 + "已停止")、
  模型 + 后端 + 运行时长副行、右侧 `预览` 次按钮 + `启动/停止推理` 主操作。
- **KPI 指标卡 ×4**:采集 FPS、推理延迟、端到端延迟、目标数(label + 大数值 + 单位 + 语义副行)。
  其中"目标数"取本帧 `detectionBuffer` 检测框数(只读);若 tracker 已暴露锁定计数则一并显示,否则仅显示检测数。
- **实时遥测曲线**:采集 FPS 最近 60 秒折线(面积 + 线 + 网格 + 时间轴 + 当前值)。
- **接收诊断面板**:发送端间隔 / 线路丢包 / 分片丢失 / 内核丢弃 / 网卡丢弃,
  值用语义色(0=绿,>0=琥珀)。

`性能统计`(StatsPage)保留在 `监控` 分组,作为更详细的全量读数;概览是 curated 概要,
两者复用同一组遥测 setter 与同一图表控件,不产生重复实现。

### 3.3 组件精修

统一强调色到 `#5E6AD2`;全站 Tabler 线性图标,**零 emoji**;统一间距/圆角/字阶。

## 4. 架构与组件边界(为隔离与可测性设计)

每个新控件是一个职责单一、接口清晰、可独立理解与测试的单元。

### 4.1 新增控件(`qt_ui/widgets/`)

- **TopNavBar**:外壳顶部栏。
  - 用途:品牌 + 主导航 + 全局操作。
  - 接口(草案):
    - `void setPrimaryItems(const QStringList& labels)`
    - `void setCurrentPrimary(int index)` / `int currentPrimary() const`
    - `void setSessionStatus(bool running, const QString& text)`
    - 信号:`primaryChanged(int)`、`saveClicked()`、`avatarClicked()`
  - 头像为占位(本期不接下拉菜单)。
  - 依赖:`IconFont`、QSS objectName(`topNav` / `primaryNavItem`)。

- **SideNav**:二级导航侧边栏(替代第二层 QTabBar)。
  - 接口:`void setItems(const QString& groupTitle, const QStringList& labels, const QStringList& iconNames)`、
    `setCurrentIndex(int)`、`int currentIndex()`;信号 `currentChanged(int)`。
  - 宽 208;白底 + 右描边;选中项 `#EEF0FC` 底 + `#4A55C8` 字 + 紫图标。图标用 `IconFont`
    渲染成 `QIcon`(单按钮无法混排图标字体与 UI 字体),选中态重新着色为紫。

- **MetricCard**:KPI 指标卡。
  - 接口:`MetricCard(label, iconName)`、`setValue(QString)`、`setUnit(QString)`、
    `setSub(QString text, Semantic sem)`(sem ∈ Neutral/Success/Warning/Danger)。

- **TelemetryChart**:把 `StatsPage` 内的 `FpsGraphWidget` 泛化为共享控件。
  - 接口:`addDataPoint(double)`、`setAccentColor(QColor)`、`setMaxPoints(int)`、
    可选 `setValueSuffix(QString)`、网格 + 末端当前值。
  - `StatsPage` 与 `OverviewPage` 共用;`FpsGraphWidget` 迁移/重命名为 `TelemetryChart`。

- **StatusPill**:状态指示(圆点 + 文本 + 语义色,**无填充背景**)。TopNav 复用。

- **CardWidget(增强,非新增)**:
  - 新增副题:`CardWidget(title, subtitle, iconName)` 或 `setSubtitle(QString)`。
  - 图标由"裸 glyph"升级为"**图标芯片**"(圆角 8 的浅底方块 + 居中图标);
    主卡用紫色调(`#EEF0FC` + 紫图标),普通卡用中性调(`#F1F1F4` + 灰图标)。
  - 保留既有 `setCollapsible` / `contentLayout` 契约不变。

### 4.2 新增页面(`qt_ui/pages/`)

- **OverviewPage**:组合 Hero + 4×MetricCard + TelemetryChart + 接收诊断面板。
  - 遥测 setter(与 StatsPage 对齐,供 `pollMonitorTelemetry` 喂数据):
    `setFps`、`setSourceFps`、`setInferenceLatency`、`setTotalLatency`、
    `setReceiverDiagnostics(...)`、`setDetectionCount(int boxes, int locked)`、
    `setSessionState(running, model, backend, uptime)`。
  - Hero 的启动/停止复用 `SessionPage::onToggleInference` 同款逻辑
    (`g_inference_session->start/stop` + `ConfigBridge::syncToRuntime`)。

### 4.3 MainWindow 改动

- 用 `TopNavBar`(一级)+ `SideNav`(二级,内容区左侧)替换 `m_primaryTabs` / `m_secondaryTabs`。
- 导航结构收敛为**单一数据源**(消除 `setupPages` 与 `onPrimaryTabChanged` 里
  `secondaryLabels` 的重复硬编码):
  ```
  概览  → [ ](OverviewPage)
  会话  → 推理启动 / 模型工具
  配置  → 画面采集 / 目标 / 硬件 / AI 模型 / 深度模型
  控制  → 瞄准热键 / 准星找色 / 寻光 / 玻璃过滤 / 宏脚本
  监控  → 性能统计 / 日志 / 自动采集 / 调试
  ```
- `保存配置` 逻辑从 MainWindow 内联按钮移到 `TopNavBar::saveClicked`(样式走 QSS,紫色,无 emoji)。
- `pollMonitorTelemetry` 增加对 `OverviewPage` 的喂数(复用现有 stats/log/autocap/debug 分支)。
- 底部 `StatusBar`:保留但**重新配色**(对齐 Token);信息精简为 配置路径 / 版本 / FPS。
  (会话状态主出口改为 TopNav 药丸,避免重复。)

### 4.4 主题与入口

- `style/theme.qss`:重构为 **Token 化样式表**——集中定义调色板、间距、圆角、字阶,
  统一强调色,新增 TopNav / 主导航项 / 二级侧边栏 / 指标卡 / 状态指示 / Hero 的样式;
  删除任何 emoji 文案来源。
- `main.cpp`:`applyLightPalette` 的 palette 对齐 Token;字体栈保留 `PingFang SC`
  并补 `Microsoft YaHei` / `Segoe UI` 回退;沿用 Fusion;保留登录与 `--shot` 截图模式。

## 5. 设计 Token(浅色)

- 强调:`accent #5E6AD2`、`hover #515DC8`、`pressed #4A55C8`、`tint #EEF0FC`、`on-tint #4A55C8`
- 中性:`page #F6F7F9`、`surface/card #FFFFFF`、`border-card #EAEAEE`、
  `border-struct #ECECEF`、`border-input #E2E2E7`
- 文本:`primary #1A1A1F`、`secondary #71717A`、`tertiary #9A9AA2`、`hint #A6A6AE`
- 语义:成功 文 `#16A34A` / 底 `#E7F6EC` / 点 `#22C55E`;警告 文 `#B45309` / 底 `#FBF0DD`;
  危险 文 `#C0362C` / 底 `#FCEBEB` / 实心 `#E5484D`
- 圆角:卡片 12、控件 8、药丸/段 7、图标芯片 8–10
- 间距:页边 16–18、卡内 15–17、卡间 13、表单行 9
- 字阶(仅 400/500 两档):品牌/数值 15–25/500、标题 14/500、正文 13、次要 12、提示 11
- 图标:Tabler outline,16–18px

## 6. 数据流

概览与统计的实时数据**几乎不新增后端**,主要复用 `MainWindow::pollMonitorTelemetry`
(200ms)既有来源:`captureFps` / `captureSourceFps` / detector 计时 / `capture*Fps` 诊断 /
`config.gpuMemoryReserveMB` / `g_inference_session`。新增的是把同一批数据也喂给 `OverviewPage`,
外加一处**只读**计数:从全局 `detectionBuffer` 读取本帧检测框数(沿用既有 version/atomic 读取模式,
不新增线程、不改跨线程契约)。

## 7. 风险与约束

- **Qt QSS 局限**:阴影需 `QGraphicsDropShadowEffect`/自绘;原型里的柔和阴影在实现时
  以"极轻描边 + 单层 drop shadow"近似,避免过度。
- **构建环境**:完整 app 需 Windows + CUDA + TensorRT;`qt_ui` 子工程仅依赖 `Qt6::Widgets`
  但 include 了 runtime 头文件,通常只能在 Windows 上构建验证。无法在本机构建时需明确说明。
- **跨线程契约**:不得新增对全局 config / detectionBuffer / 帧缓冲的非同步访问;
  遥测读取沿用既有 atomic / `configMutex` 模式。
- **新增 `.cpp` 自动纳入构建**:`qt_ui/CMakeLists.txt` 用 `GLOB_RECURSE`,新增文件即被编译,
  无需改 CMake(但新增后需重跑 CMake 配置)。

## 8. 验证

- 复用 `main.cpp` 既有 `--shot` 截图模式:扩展 shots 列表加入 `概览`,
  在 Windows 上生成各页截图人工核对视觉。
- 关键交互手测:主/次导航切换、保存配置、Hero 启停推理、概览实时数值随会话刷新。
- 视觉对照:以本规格第 3 节与已确认原型为基准。

## 9. 实现阶段(高层;细节进入实现计划)

1. Token 化 `theme.qss` + `main.cpp` palette/字体对齐(地基,无功能变更)。
2. 基础控件:`StatusPill`(无背景)、`SideNav`、`MetricCard`、`TelemetryChart`(由 `FpsGraphWidget` 泛化)、`CardWidget` 增强。
3. `TopNavBar` + MainWindow 外壳替换(导航单一数据源、保存迁移、底部 StatusBar 重配色)。
4. `OverviewPage` 概览仪表盘 + 接入 `pollMonitorTelemetry`。
5. 全站 emoji 清除与一致性收尾;`--shot` 扩展与视觉核对。

## 10. Mac 预览骨架(已落地)

为了在 Mac 上直接看 / 点新设计(完整 app 因 runtime 耦合无法在 Mac 构建),新增独立预览工程:

- 位置:`qt_ui/preview/`(`preview_main.cpp` / `PreviewWindow.{h,cpp}` / `preview.qss` / 独立 `CMakeLists.txt`)。
- 仅依赖 `Qt6::Widgets`,**显式列出源**(不 GLOB),编译新组件 + `OverviewPage` + `IconFont`,无 runtime、无登录。
- 用假数据(250ms `QTimer`)驱动概览仪表盘;可点一级 / 二级导航、可切会话启停状态。
- 新组件(`TopNavBar` / `SideNav` / `MetricCard` / `TelemetryChart` / `StatusPill` / `OverviewPage`)是
  **正式实现**,后续正式 `MainWindow` 直接复用;预览程序只是它们的驱动壳与 Mac 验证台。
- 构建:`cmake -S qt_ui/preview -B qt_ui/preview/build -G Ninja && cmake --build qt_ui/preview/build`;
  运行加 `--shot` 生成 `/tmp/apo_preview/*.png` 供无头核对。
- 约束:这些组件为保持 Mac 可预览,**不得**直接 include runtime 头或访问全局;runtime 接线只在
  `MainWindow` 侧通过 setter / 信号完成(`OverviewPage` 已解耦:setter 喂数 + `startStopRequested` 信号)。
