#ifndef APOTHEOSIS_H
#define APOTHEOSIS_H

#include <atomic>
#include <mutex>

#include "config.h"
#include "i_detector.h"
#include "trt_detector.h"
#include "dml_detector.h"
#include "mouse.h"
#include "Arduino.h"
#include "ghub.h"
#include "detection_buffer.h"
#include "KmboxNetConnection.h"
#include "KmboxAConnection.h"
#include "Makcu.h"

namespace runtime { class InferenceSession; }

extern Config config;
extern TrtDetector trt_detector;
extern DirectMLDetector* dml_detector;
extern IDetector* g_detector;
extern runtime::InferenceSession* g_inference_session;
extern DetectionBuffer detectionBuffer;
extern MouseThread* globalMouseThread;
extern GhubMouse* gHub;
extern Arduino* arduinoSerial;
extern KmboxNetConnection* kmboxNetSerial;
extern KmboxAConnection* kmboxASerial;
extern MakcuConnection* makcuSerial;
extern std::atomic<bool> input_method_changed;
// `aiming` is a convenience mirror of "any aim hotkey pressed"; for the
// actual active profile index consult runtime::g_active_hotkey_index.
extern std::atomic<bool> aiming;

// Session-scoped stop signal: toggled true by InferenceSession::stop() so the
// capture and mouse threads can exit their loops without tearing down the whole
// app (which is the role of the global shouldExit flag).
extern std::atomic<bool> session_stop_requested;
extern std::recursive_mutex configMutex;

// Aim telemetry: latest crosshair-to-target error (in detection pixels).
extern std::atomic<float> g_pid_last_err_px;
// Flick/Track telemetry: false = Flick gains active, true = Track gains active.
extern std::atomic<bool>  g_pid_mode_track;

// Set by flashlight_runtime each frame: true when the 寻光 depth-gate is active
// (an active hotkey has flashlight detection on AND 抗误锁 is high enough that
// depth.mode > 0). The capture thread ORs it into depthNeeded / produce_normalized
// so the normalized depth map exists for the gate to sample. Still gated by the
// master depth_inference_enabled switch.
extern std::atomic<bool>  g_flashlight_depth_required;

void createInputDevices();
void assignInputDevices();

// Dynamic-FOV telemetry. Effective half-diameters around the crosshair
// (detection pixels) the tracker is currently using to gate candidates.
// 0 means "disabled / no gate". The overlay reads these to draw the aim
// region indicator. Updated each detection frame by mouse_thread_loop.
extern std::atomic<float> g_dynamic_fov_radius_x_px;
extern std::atomic<float> g_dynamic_fov_radius_y_px;

#endif // APOTHEOSIS_H
