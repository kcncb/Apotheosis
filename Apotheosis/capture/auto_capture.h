#ifndef CAPTURE_AUTO_CAPTURE_H
#define CAPTURE_AUTO_CAPTURE_H

#include <atomic>

// Auto data-collection harness.
//
// Wakes on every detection publication. Two acquisition modes:
//
//   - FORCE  : while any of the configured force-keys (typically a mouse
//              side button) is held, dump every fresh frame regardless
//              of confidence. Subject to the cooldown.
//   - AUTO   : when the force-key is not held, save a frame iff at least
//              one detection's confidence lies in the configured zone:
//                * "use_high" enabled AND conf > high_conf  → save (clean,
//                  high-confidence sample useful for ground truth)
//                * "use_low"  enabled AND conf < low_conf   → save (hard
//                  example useful for active-learning labeling)
//              Both can be on simultaneously — either trigger fires.
//
// Output: BGR detection-resolution frame as JPG + matching YOLO-format
// .txt label (one row per detection that passed the model's NMS this
// frame, regardless of which one triggered the save).
//
// Files land in config.auto_capture_output_dir (default "screenshots/auto"),
// named with a wall-clock timestamp + a 3-digit counter for sub-ms dedupe.
namespace AutoCapture
{

// Stats surfaced to the UI's monitor page.
extern std::atomic<int>  g_saved_total;
extern std::atomic<int>  g_saved_session;   // resets each run
extern std::atomic<bool> g_force_held;
extern std::atomic<bool> g_running;

// Entry point for the background thread (StartThreadGuarded wraps this).
void auto_capture_thread();

// Reset the session counter (e.g. UI "clear" button).
void reset_session_counter();

} // namespace AutoCapture

#endif // CAPTURE_AUTO_CAPTURE_H
