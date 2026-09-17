#ifndef APOTHEOSIS_H
#define APOTHEOSIS_H

#include <atomic>
#include <mutex>

#include "config.h"
#include "i_detector.h"
#include "trt_detector.h"
#include "mouse.h"
#include "detection_buffer.h"
#include "Makcu.h"
#include "MakcuNew.h"

namespace runtime { class InferenceSession; }

extern Config config;
extern TrtDetector trt_detector;
extern IDetector* g_detector;
extern runtime::InferenceSession* g_inference_session;
extern DetectionBuffer detectionBuffer;
extern MakcuConnection* makcuSerial;
extern MakcuNewConnection* makcuNewSerial;
// ★ KMBOXNET 后端。原先是 Apotheosis.cpp 的文件级全局, 2026-09-17 提到头文件 ——
//   因为通用控制器层(runtime/aim_loop.cpp)要拿它构造自己的 MouseThread。
class KmboxNetConnection;
extern KmboxNetConnection* kmboxNetSerial;
extern std::atomic<bool> input_method_changed;
// `aiming` is a convenience mirror of "any aim hotkey pressed"; for the
// actual active profile index consult runtime::g_active_hotkey_index.
extern std::atomic<bool> aiming;

// Session-scoped stop signal: toggled true by InferenceSession::stop() so the
// capture and mouse threads can exit their loops without tearing down the whole
// app (which is the role of the global shouldExit flag).
extern std::atomic<bool> session_stop_requested;
extern std::recursive_mutex configMutex;
extern std::mutex inputDeviceMutex;

// ★ 瞄准遥测已随控制链删除(2026-09-17): g_pid_last_err_px / g_pid_mode_track /
//   g_dynamic_fov_radius_x_px / g_dynamic_fov_radius_y_px 原本都由
//   mouse_thread_loop.cpp 每拍写入, 那个文件已删除。它们的读取点(预览窗的
//   动态 FOV 椭圆、DebugPage 的读数)也一并删掉了 —— 留下"没有生产者的读数"
//   会让界面显示一个永远不动、看起来像功能没生效的数字。

void createInputDevices();
void assignInputDevices();

#endif // APOTHEOSIS_H
