#include "../boss_aim.h"
#include "../aim_path.h"
#include "../ava_exact/pidf_mode1_exact.hpp"
#include <cmath>
#include <iostream>
#include <cstdlib>
using namespace motion;
#define CHECK(x) do { if (!(x)) {std::cerr<<"FAIL " #x << ':' << __LINE__ << '\n';std::exit(1);} } while(0)
boss::EngineInput input(std::vector<cv::Rect2f>& boxes, std::vector<int>& cls,std::vector<float>& conf,Time captured,Time now,uint64_t seq) {
 boss::EngineInput in;in.boxes=&boxes;in.classes=&cls;in.confidences=&conf;in.target_slots={{0,.5,.5,.1}};
 in.crosshair_x=in.crosshair_y=160;in.fov_radius_x=in.fov_radius_y=150;in.image_size=320;
 in.pidf_params.kp_x=in.pidf_params.kp_y=2;in.pidf_params.kd_x=in.pidf_params.kd_y=.05;
 in.pidf_params.kf_x=in.pidf_params.kf_y=1;in.pidf_params.lr_x=in.pidf_params.lr_y=.08;
 in.captured_ns=captured;in.now_ns=now;in.sequence=seq;in.calibration={1,1,5'000'000};return in;
}
int main() {
 auto journal=std::make_shared<CommandJournal>();boss::AimEngine engine(journal);OutputMapper mapper;
 std::vector<cv::Rect2f> boxes{cv::Rect2f(210,136,20,48)};std::vector<int> cls{0};std::vector<float> conf{.9};
 Time t=1'000'000'000; auto in=input(boxes,cls,conf,t,t+16'000'000,1);auto out=engine.tick(in,1./120);
 CHECK(out.have_target&&out.control_due);CHECK(out.pixel_dx>0);int id=out.current_track_id;
 // A large camera movement must not be misidentified as a new target.
 const auto command=journal->queued(30,0,in.calibration,t+16'000'000);journal->complete(command,true,t+16'000'000);
 boxes[0].x-=30;t+=24'000'000;in=input(boxes,cls,conf,t,t+16'000'000,2);out=engine.tick(in,.024);
 CHECK(out.have_target);CHECK(out.current_track_id==id);
 CHECK(std::abs(out.anchor.x-190)<3);
 in.has_new_measurement=false;in.now_ns+=1'000'000;out=engine.tick(in,1./120);
 CHECK(!out.control_due);in.now_ns+=9'000'000;out=engine.tick(in,1./120);
 CHECK(out.have_target&&out.control_due&&out.coasting);
 // Stale streams stop prediction; an empty-detection frame is not a fresh anchor.
 in.now_ns+=200'000'000;out=engine.tick(in,1./120);CHECK(!out.have_target);
 engine.reset(); t+=300'000'000;in=input(boxes,cls,conf,t,t+16'000'000,3);out=engine.tick(in,1./120);
 CHECK(out.have_target);boxes.clear();cls.clear();conf.clear();t+=8'000'000;
 in=input(boxes,cls,conf,t,t+16'000'000,4);out=engine.tick(in,1./120);
 CHECK(out.have_target&&out.coasting);
 const int held=out.current_track_id;
 boxes={cv::Rect2f(180,136,20,48)};cls={0};conf={.9};t+=8'000'000;
 in=input(boxes,cls,conf,t,t+16'000'000,5);out=engine.tick(in,1./120);
 CHECK(out.have_target&&out.current_track_id==held&&!out.coasting);
 // Invalid observations do not erase the last valid estimate.
 in.sequence=6;in.captured_ns-=100'000'000;in.now_ns+=10'000'000;
 out=engine.tick(in,1./120);CHECK(out.have_target&&out.coasting);
 // Path shaping keeps fractions; the actuator owns the only integer rounding.
 boss::AimPathDriver path;auto shaped=path.step(190,160,160,160,.008,1,.25,.1);
 CHECK(shaped.move_x==.25&&shaped.move_y==.1);
 int sum=0;for(int k=0;k<4;++k)sum+=mapper.map({shaped.move_x,0},{},0,0)[0];CHECK(sum==1);
 // Legacy PIDF's two-axis initialization regression remains covered.
 cvm::recovered::PidfConfig pc{};pc.kp_x=pc.kp_y=2;pc.kd_x=pc.kd_y=.05;pc.kf_x=pc.kf_y=1;pc.lr_x=pc.lr_y=.08;
 auto state=cvm::recovered::construct_pidf_mode1(pc,0);cvm::recovered::PidfDelayModelExact delay{};delay.measure_latency_sec=2./120;
 cvm::recovered::PidfInputExact pi{};pi.valid=1;pi.target_x=pi.target_y=260;pi.current_x=pi.current_y=160;pi.radius_x=pi.radius_y=20;
 for(int f=1;f<10;++f) {auto r=cvm::recovered::update_pidf_mode1(state,delay,pi,f/120.);CHECK(r.dx==r.dy);}

 // Closed-loop integration uses the production selector, observer, planner,
 // path shaper, output mapper and journal, with delayed synthetic camera frames.
 {
   auto j=std::make_shared<CommandJournal>();boss::AimEngine e(j);OutputMapper map;
   std::vector<cv::Rect2f> bs;std::vector<int> cs{0};std::vector<float> fs{.9};
   struct Sample {Time t;double p;uint64_t seq;};std::deque<Sample> camera;
   double position=60;Time next=2'000'000'000;uint64_t seq=0,done=0;int moves=0,changes=0,last=-1;
   auto tick=input(bs,cs,fs,next,next,0);tick.has_new_measurement=false;
   for(int ms=0;ms<1200;++ms) {
     const Time now=2'000'000'000+ms*1'000'000LL;
     const double velocity=ms<300?300:ms<650?-300:0;
     position+=velocity*.001;
     for(const auto& c:j->snapshot())if(c.id>done&&c.state==Delivery::sent&&c.effect<=now){position-=c.pixels.x;done=c.id;}
     if(now>=next){++seq;if(seq%13)camera.push_back({now,position,seq});next=now+8'000'000;}
     tick.has_new_measurement=false;tick.now_ns=now;
     if(!camera.empty()&&camera.front().t+16'000'000<=now) {
       const auto sample=camera.front();camera.pop_front();
       bs={cv::Rect2f(float(150+sample.p),136,20,48)};
       tick=input(bs,cs,fs,sample.t,now,sample.seq);
     }
     const auto command=e.tick(tick,1./120);
     if(command.have_target&&command.current_track_id!=last){++changes;last=command.current_track_id;}
     if(command.have_target&&command.control_due) {
       auto counts=map.map({command.pixel_dx,command.pixel_dy},tick.calibration,0,0);
       if(counts[0]||counts[1]){auto token=j->queued(counts[0],counts[1],tick.calibration,now);j->complete(token,true,now);++moves;}
     }
     CHECK(std::isfinite(position)&&std::abs(position)<150);
   }
   std::cout<<"engine trajectory residual="<<position<<" target generations="<<changes<<'\n';
   CHECK(std::abs(position)<3);CHECK(moves>12);CHECK(changes<=2);
 }
 std::cout<<"AimEngine integration passed\n";
}
