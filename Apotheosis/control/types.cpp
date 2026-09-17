#include "types.h"

#include <cmath>

namespace control {

double Vec2::norm() const
{
    return std::sqrt(x * x + y * y);
}

double Box::diagonal() const
{
    return std::sqrt(w * w + h * h);
}

} // namespace control
