#ifndef MOUSE_MOVERS_H
#define MOUSE_MOVERS_H

namespace mover
{

// AVA PIDF Mode 1 的原生 96-byte 配置字段。
struct PidfParams
{
    double kp_x = 1.0, kp_y = 1.0;
    double ki_x = 0.0, ki_y = 0.0;
    double kd_x = 0.01, kd_y = 0.01;
    double kf_x = 0.0, kf_y = 0.0;
    double lr_x = 0.0, lr_y = 0.0;
    int deadzone_x = 0, deadzone_y = 0;
    int movement_limit_x = 0, movement_limit_y = 0;
};

} // namespace mover

#endif // MOUSE_MOVERS_H
