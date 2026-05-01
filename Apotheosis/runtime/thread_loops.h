#pragma once

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#include "AimbotTarget.h"

class MouseThread;

extern std::mutex g_trackerDebugMutex;
extern std::vector<TrackDebugInfo> g_trackerDebugTracks;
extern int g_trackerLockedId;

void mouseThreadFunction(MouseThread& mouseThread);
