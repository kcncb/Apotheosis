#include "tracking_scenarios.h"
#include <cstdio>
#include <cstdlib>
int main(int argc, char** argv)
{
    tracking_test::Environment e;
    if (argc>1) e.output_hz=std::atoi(argv[1]);
    if (argc>2) e.measurement_ms=std::atoi(argv[2]);
    if (argc>3) e.physical_gain=std::atof(argv[3]);
    if (argc>4) e.camera_hz=std::atoi(argv[4]);
    if (argc>5) e.noise=std::atof(argv[5]);
    if (argc>6) e.jitter=std::atoi(argv[6])!=0;
    e.model_gain=argc>7?std::atof(argv[7]):e.physical_gain;
    if (argc>8) e.response_offset_ms=std::atoi(argv[8]);
    if (argc>9) e.speed_scale=std::atof(argv[9]);
    if(e.output_hz<30||e.output_hz>250||e.camera_hz<30||e.camera_hz>1000||e.measurement_ms<0||e.measurement_ms>100
       ||!std::isfinite(e.physical_gain)||!std::isfinite(e.model_gain)||!std::isfinite(e.noise)||e.physical_gain<=0||e.model_gain<=0||e.noise<0||e.response_offset_ms< -4||e.response_offset_ms>50||!std::isfinite(e.speed_scale)||e.speed_scale<=0||e.speed_scale>3) return 2;
    std::puts("scenario,output_hz,measurement_ms,gain,camera_hz,noise,jitter,response_error_ms,speed_scale,p95_px,peak_px,within_3px_pct,error_integral_px_ms,commands");
    for(int scenario=0;scenario<9;++scenario)
    {
        const auto m=tracking_test::simulate(scenario,e);
        std::printf("%s,%d,%d,%.2f,%d,%.2f,%d,%d,%.2f,%.3f,%.3f,%.3f,%.1f,%d\n",tracking_test::kNames[scenario],
                    e.output_hz,e.measurement_ms,e.physical_gain,e.camera_hz,e.noise,int(e.jitter),e.response_offset_ms,e.speed_scale,
                    m.p95,m.peak,m.within_three_percent,m.error_integral,m.commands);
    }
}
