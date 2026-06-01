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

// Smart-trigger telemetry surfaced by MouseThread::updateSmartTrigger.
//   g_smart_trigger_ready              -> true while the left button is held
//   g_smart_trigger_hit_prob           -> on-target fraction [0,1] (1 = dead-centre)
//   g_smart_trigger_recent_variance_px -> current on-target dwell time in ms
// (the third name is kept for ABI/compat; it now carries dwell-ms, not RMS).
extern std::atomic<bool>  g_smart_trigger_ready;
extern std::atomic<float> g_smart_trigger_hit_prob;
extern std::atomic<float> g_smart_trigger_recent_variance_px;

// Aim telemetry: latest crosshair-to-target error (in detection pixels).
extern std::atomic<float> g_pid_last_err_px;
// Flick/Track telemetry: false = Flick gains active, true = Track gains active.
extern std::atomic<bool>  g_pid_mode_track;
// Set by the mouse thread when the active hotkey profile needs the normalized
// depth map for threat scoring. The capture thread reads it to decide whether
// to run depth inference / produce the normalized map even when no depth
// display option is on.
extern std::atomic<bool>  g_threat_depth_required;

void createInputDevices();
void assignInputDevices();

// Dynamic-FOV telemetry. Effective half-diameters around the crosshair
// (detection pixels) the tracker is currently using to gate candidates.
// 0 means "disabled / no gate". The overlay reads these to draw the aim
// region indicator. Updated each detection frame by mouse_thread_loop.
extern std::atomic<float> g_dynamic_fov_radius_x_px;
extern std::atomic<float> g_dynamic_fov_radius_y_px;

#endif // APOTHEOSIS_H
