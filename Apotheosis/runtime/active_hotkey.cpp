#include "active_hotkey.h"

namespace runtime
{

std::atomic<int> g_active_hotkey_index{ -1 };
std::mutex g_model_metadata_mutex;
detector::ModelMetadata g_model_metadata{};

} // namespace runtime
