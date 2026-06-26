#include "AimbotTarget.h"

// The legacy MultiTargetTracker / threat-scoring / AimbotTarget pipeline that
// used to live here has been removed: the live aim path is boss::AimEngine
// (mouse/boss_aim.cpp + runtime/mouse_thread_loop.cpp). The only remaining
// public symbol from this unit is TrackDebugInfo, which is a header-only POD,
// so this translation unit has no out-of-line definitions. It is kept (and
// still listed in the build) so AimbotTarget.h is verified to self-compile.
