#pragma once

#include <memory>

class Config;

namespace runtime_config
{
// Atomic shared_ptr operations are used for C++17 compatibility. Readers never
// lock and retain a self-contained immutable Config for the whole frame.
std::shared_ptr<const Config> read();
void publish();
}
