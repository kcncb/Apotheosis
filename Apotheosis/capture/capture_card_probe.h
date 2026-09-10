#pragma once

// =============================================================================
// 采集卡探测门面 (无 CUDA / OpenCV / TensorRT 依赖)
// =============================================================================
//
// Qt UI 需要"枚举采集卡 + 拿到每张卡的真实能力表", 但不应该为此把 CUDA /
// OpenCV / TensorRT 头文件全拖进 UI 编译单元。所以这里只暴露一个薄门面:
// 声明在无依赖的头文件里, 实现在 mf_capture.cpp (那里本来就有 Media Foundation)。
//
// 用法 (UI 侧):
//     auto devs = capture_card::ProbeAll();          // 枚举 + 探测, 慢, 只在刷新时调
//     auto dev  = capture_card::FindByName(devs, cfg.capture_device);
//     auto fmts = mfcap::Formats(*dev);              // 三级联动第一级
//     auto res  = mfcap::Resolutions(*dev, fmt);     // 第二级
//     auto fps  = mfcap::FpsList(*dev, fmt, w, h);   // 第三级

#include "capture/capture_card_caps.h"

#include <string>
#include <vector>

namespace capture_card
{

// 枚举本机所有视频采集设备并探测各自的能力表。
// 每个设备 20-300ms, 只在"打开采集页 / 点刷新 / 换设备"时调用, 不要放进热路径。
std::vector<MFDeviceInfo> ProbeAll();

// 只探测指定序号的设备 (其余设备保留名称, caps 留空)。
std::vector<MFDeviceInfo> ProbeOne(int device_index);

// 按 friendly name 找设备 (配置里存的是名字, 因为 index 会随插拔顺序变化)。
// 找不到返回 nullptr —— 调用方必须处理"上次选的卡没插"这种情况,
// 而不是退回到第 0 张卡。
const MFDeviceInfo* FindByName(const std::vector<MFDeviceInfo>& devs,
                               const std::string& friendly_name);

} // namespace capture_card
