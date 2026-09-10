// =============================================================================
// capture_card_caps.h 自测 —— 三级联动 + 选优 + 配置吸附
// =============================================================================
//
// 纯逻辑, 无 Windows / CUDA / OpenCV 依赖, 因此在任何平台可编译运行:
//
//   c++ -std=c++20 -I Apotheosis Apotheosis/capture/tests/capture_card_caps_test.cpp -o caps_test
//
// 最重要的用例是 [1]: 它就是用户提出的那个场景 ——
//   "nv12 我的采集卡不支持 1k 240 但是 mjpeg 支持, 那我选 nv12 选 1k 的时候
//    就不要展示出 240, 展示真实的可选项"
// =============================================================================

#include "capture/capture_card_caps.h"

#include <cstdio>
#include <string>
#include <vector>

static int g_failures = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) { printf("  [FAIL] %s\n", msg); ++g_failures; }          \
        else printf("  [ ok ] %s\n", msg);                                     \
    } while (0)

static std::string joinInts(const std::vector<int>& v)
{
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) { if (i) s += ","; s += std::to_string(v[i]); }
    return s;
}

static std::string joinRes(const std::vector<std::pair<int,int>>& v)
{
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += ",";
        s += std::to_string(v[i].first) + "x" + std::to_string(v[i].second);
    }
    return s;
}

// 构造一块"用户描述的那种卡":
//   NV12  只到 1080p60 / 720p120
//   MJPG  能到 1080p240 / 720p240
//   YUY2  只到 720p60
//   H264  有, 但本程序不支持
static MFDeviceInfo makeCard()
{
    MFDeviceInfo d;
    d.index = 0;
    d.name = "设备 #0 - Fake HDMI Card";
    d.friendly_name = "Fake HDMI Card";
    d.caps_probed = true;

    auto add = [&](const char* f, int w, int h, std::vector<int> fps, bool sup) {
        MFCapability c; c.format = f; c.width = w; c.height = h; c.fps = fps; c.supported = sup;
        d.caps.push_back(c);
    };
    add("NV12", 1920, 1080, { 60 },              true);
    add("NV12", 1280,  720, { 60, 120 },         true);
    add("MJPG", 1920, 1080, { 60, 120, 240 },    true);
    add("MJPG", 1280,  720, { 60, 120, 240 },    true);
    add("YUY2", 1280,  720, { 60 },              true);
    add("H264", 1920, 1080, { 60 },              false);
    return d;
}

int main()
{
    printf("=== capture_card_caps 自测 ===\n\n");
    const MFDeviceInfo card = makeCard();

    // ---------------------------------------------------------------- [1]
    printf("[1] 三级联动: 选 NV12 时 1080p 不得出现 240 (用户的原始场景)\n");
    {
        const auto formats = mfcap::Formats(card);
        printf("      格式: %s\n", [&]{ std::string s; for (auto& f : formats) { if (!s.empty()) s += ","; s += f; } return s; }().c_str());
        CHECK(formats.size() == 3, "只列出 3 个可用格式 (H264 被排除)");
        CHECK(formats[0] == "NV12", "未压缩优先, NV12 排第一");

        // 选 NV12 -> 分辨率只列 NV12 真实有的
        const auto nv12_res = mfcap::Resolutions(card, "NV12");
        printf("      NV12 分辨率: %s\n", joinRes(nv12_res).c_str());
        CHECK(nv12_res.size() == 2, "NV12 只有 2 档分辨率");

        // ★ 核心: NV12 + 1080p 的帧率列表
        const auto nv12_1080 = mfcap::FpsList(card, "NV12", 1920, 1080);
        printf("      NV12 1920x1080 帧率: %s\n", joinInts(nv12_1080).c_str());
        CHECK(nv12_1080 == std::vector<int>{ 60 }, "NV12@1080p 只有 60, 没有 240");

        // 同一分辨率换成 MJPG, 240 才出现
        const auto mjpg_1080 = mfcap::FpsList(card, "MJPG", 1920, 1080);
        printf("      MJPG 1920x1080 帧率: %s\n", joinInts(mjpg_1080).c_str());
        CHECK(mjpg_1080 == std::vector<int>({ 60, 120, 240 }), "MJPG@1080p 有 240");

        // 换分辨率后帧率列表同样跟着变
        CHECK(mfcap::FpsList(card, "NV12", 1280, 720) == std::vector<int>({ 60, 120 }),
              "NV12@720p 是 60/120 (与 1080p 不同)");

        // 不存在的组合必须返回空, 而不是编一个出来
        CHECK(mfcap::FpsList(card, "NV12", 3840, 2160).empty(), "不支持的组合返回空表");
        CHECK(mfcap::Resolutions(card, "H264").size() == 1, "未支持格式仍如实保留在能力表里");
    }

    // ---------------------------------------------------------------- [2]
    printf("\n[2] 选优: 同等帧率优先未压缩, 达不到才退 MJPG\n");
    {
        std::string f, why; int w = 0, h = 0, fps = 0;

        // 目标 1080p120: NV12 在 1080p 只有 60 -> 必须退到 MJPG@1080p,
        // 而不是拿 NV12@720p120 来"凑帧率"(那会偷偷换掉分辨率)
        CHECK(mfcap::PickBest(card, 1920, 1080, 120, f, w, h, fps, &why), "1080p120 能选出组合");
        printf("      %s\n", why.c_str());
        CHECK(f == "MJPG" && w == 1920 && h == 1080 && fps >= 120,
              "1080p120 -> MJPG@1080p (分辨率必须守住, 不许换成 720p)");

        // 反向确认: 没有精确分辨率可选时, 才允许退到最接近的尺寸
        MFDeviceInfo loose;
        loose.caps_probed = true;
        { MFCapability c; c.format="NV12"; c.width=1280; c.height=720; c.fps={120}; c.supported=true; loose.caps.push_back(c); }
        { MFCapability c; c.format="MJPG"; c.width=1280; c.height=720; c.fps={120}; c.supported=true; loose.caps.push_back(c); }
        CHECK(mfcap::PickBest(loose, 1920, 1080, 120, f, w, h, fps, &why), "无精确分辨率时仍能选优");
        printf("      %s\n", why.c_str());
        CHECK(f == "NV12", "分辨率无法精确匹配时, 才由未压缩格式胜出");
        CHECK(why.find("720x720") == std::string::npos && why.find("最接近") != std::string::npos,
              "降级到非精确分辨率时, reason 必须明说");

        // 目标 720p120: NV12 能做到 -> 用 NV12, 不用 MJPG
        CHECK(mfcap::PickBest(card, 1280, 720, 120, f, w, h, fps, &why), "720p120 能选出组合");
        printf("      %s\n", why.c_str());
        CHECK(f == "NV12" && fps == 120, "720p120 -> NV12 (未压缩优先)");

        // 目标 1080p60: NV12 能做到 -> NV12
        CHECK(mfcap::PickBest(card, 1920, 1080, 60, f, w, h, fps, &why), "1080p60 能选出组合");
        CHECK(f == "NV12" && w == 1920 && h == 1080, "1080p60 -> NV12 1920x1080");

        // 目标 1080p300: 设备没有, 取最高可用帧率
        CHECK(mfcap::PickBest(card, 1920, 1080, 300, f, w, h, fps, &why), "1080p300 能选出组合");
        printf("      %s\n", why.c_str());
        CHECK(fps == 240, "目标 300fps 时取设备最高的 240");
    }

    // ---------------------------------------------------------------- [3]
    printf("\n[3] 配置校验: 失效配置必须被【拒绝】, 不许偷偷换成别的模式\n");
    {
        // 用户在 NV12 下挑了 1080p240 —— 设备根本不支持, 必须拒绝
        CHECK(!mfcap::Validate(card, "NV12", 1920, 1080, 240),
              "NV12 1080p240 -> 拒绝 (设备只有 60)");
        CHECK(mfcap::Validate(card, "NV12", 1920, 1080, 60),
              "NV12 1080p60  -> 通过");
        CHECK(mfcap::Validate(card, "MJPG", 1920, 1080, 240),
              "MJPG 1080p240 -> 通过");
        CHECK(!mfcap::Validate(card, "MJPG", 1920, 1080, 300),
              "MJPG 1080p300 -> 拒绝 (设备最高 240)");
        CHECK(!mfcap::Validate(card, "H264", 1920, 1080, 60),
              "H264 -> 拒绝 (本程序不解码该格式)");
        CHECK(!mfcap::Validate(card, "MJPG", 3840, 2160, 60),
              "不存在的分辨率 -> 拒绝");

        // 拒绝时必须能说出"设备到底支持什么", 否则用户无从下手
        const auto allowed = mfcap::FpsList(card, "NV12", 1920, 1080);
        CHECK(allowed == std::vector<int>{ 60 }, "拒绝后能给出现实可选项: NV12@1080p 只有 60");
        CHECK(mfcap::Formats(card) == std::vector<std::string>({ "NV12", "YUY2", "MJPG" }),
              "拒绝后能给出该设备全部可用格式");
    }

    // ---------------------------------------------------------------- [4]
    printf("\n[4] 帧率容差 (29.97 / 59.94)\n");
    {
        MFDeviceInfo nt;
        MFCapability c; c.format = "NV12"; c.width = 1280; c.height = 720;
        c.fps = { 30, 60 }; c.supported = true;
        nt.caps.push_back(c);
        nt.caps_probed = true;

        CHECK(mfcap::Supports(nt, "NV12", 1280, 720, 30), "30 精确命中");
        CHECK(mfcap::Supports(nt, "NV12", 1280, 720, 29), "29 落在 ±1 容差内");
        CHECK(!mfcap::Supports(nt, "NV12", 1280, 720, 120), "120 不支持");
        CHECK(!mfcap::Supports(nt, "NV12", 1920, 1080, 30), "分辨率不符即不支持");
    }

    // ---------------------------------------------------------------- [5]
    printf("\n[5] 空能力表 / 退化设备\n");
    {
        MFDeviceInfo empty;
        empty.caps_probed = true;
        CHECK(mfcap::Formats(empty).empty(), "空设备无格式");
        CHECK(mfcap::Formats(empty).empty(), "空设备无可用格式");
        CHECK(!mfcap::Validate(empty, "NV12", 1920, 1080, 60), "空设备校验一律失败");
        std::string fmt; int w2 = 0, h2 = 0, fps2 = 0;
        CHECK(!mfcap::PickBest(empty, 1920, 1080, 60, fmt, w2, h2, fps2), "空设备选优失败");
        CHECK(mfcap::Describe(empty) == "(device reported no video capabilities)", "空设备描述合理");

        MFDeviceInfo unprobed;
        CHECK(mfcap::Describe(unprobed) == "(not probed)", "未探测设备描述合理");
    }

    printf("\n=====================================\n");
    if (g_failures) { printf("失败 %d 项\n", g_failures); return 1; }
    printf("结果: 全部通过\n");
    return 0;
}
