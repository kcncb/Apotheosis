#ifndef RUNTIME_ACTIVE_HOTKEY_H
#define RUNTIME_ACTIVE_HOTKEY_H

#include <atomic>
#include <mutex>

#include "detector/model_inspector.h"

namespace runtime
{

// Index of the Config::hotkeys entry whose keys are currently pressed.
// -1 means no aim hotkey is active. Updated by the keyboard listener,
// consumed by the mouse loop.
extern std::atomic<int> g_active_hotkey_index;

// Snapshot of class names / count sourced from the last successfully loaded
// model. UI panels read this to render the class filter table.
extern std::mutex g_model_metadata_mutex;
extern detector::ModelMetadata g_model_metadata;

} // namespace runtime

#endif // RUNTIME_ACTIVE_HOTKEY_H
