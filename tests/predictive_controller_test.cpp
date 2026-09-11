#include "mouse/control/predictive_controller.h"
#include <cmath>
#include <iostream>
#include <vector>
#include <algorithm>
#include <cstdlib>
using namespace motion;
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL " #x << ':' << __LINE__ << '\n'; std::exit(1); } } while (0)
struct Event { Time time; Vec anchor; uint64_t sequence; };
struct Result { double tail, peak, settle; };
Result simulate(double gain, double speed, bool jitter, bool rejected, bool vertical=false, bool maneuver=false, Time extra_delay=0)
{
    auto journal=std::make_shared<CommandJournal>(); PredictiveController controller(journal);
    ControlConfig c;c.calibration={gain,gain,5'000'000};controller.configure(c);OutputMapper mapper;
    Vec position{60,60}; const Time start=1'000'000'000;
    std::vector<Event> frames;uint64_t sequence=0;Time nextFrame=start;
    std::vector<double> errors;double peak=0,settle=-1;uint64_t applied=0;int issued=0;
    for(int ms=0;ms<2000;++ms)
    {
        const Time now=start+ms*1'000'000LL;
        const double current_speed = !maneuver ? speed : ms<500 ? speed : ms<1000 ? -speed : 0;
        if(ms>0) {if(vertical)position.y+=current_speed*.001;else position.x+=current_speed*.001;}
        for(const auto& cmd:journal->snapshot()) if(cmd.id>applied && cmd.state==Delivery::sent && cmd.effect+extra_delay<=now)
        {position.x-=cmd.pixels.x;position.y-=cmd.pixels.y;applied=cmd.id;}
        if(now>=nextFrame)
        {
            ++sequence;
            const double noise=(int(sequence%3)-1)*.35;
            if(!(jitter && sequence%13==0)) frames.push_back({now,{position.x+noise,position.y-noise},sequence});
            nextFrame=now+(jitter && sequence%7==0?12'000'000:8'000'000);
        }
        while(!frames.empty() && frames.front().time+16'000'000<=now)
        {const auto e=frames.front();frames.erase(frames.begin());controller.observe({e.sequence,e.time,e.anchor,1},now);}
        auto out=controller.advance(now,{});
        if(out.valid&&out.due)
        {
            auto counts=mapper.map(out.pixels,c.calibration,0,0);
            if(counts[0]||counts[1]) {
                auto id=journal->queued(counts[0],counts[1],c.calibration,now);
                journal->complete(id,!(rejected && ++issued%7==0),now);
            }
        }
        double error=std::hypot(position.x,position.y);peak=std::max(peak,error);
        if(ms>(maneuver?1500:1000))errors.push_back(error);
        if(settle<0 && error<3)settle=ms;
        CHECK(std::isfinite(error)&&error<500);
    }
    std::sort(errors.begin(),errors.end());
    return {errors[errors.size()*95/100],peak,settle};
}
int main()
{
    for(double gain:{.5,1.0,2.0}) for(double speed:{0.0,250.0,750.0})
    {
        auto r=simulate(gain,speed,true,true);
        std::cout<<"g="<<gain<<" speed="<<speed<<" tail="<<r.tail<<" first="<<r.settle<<'\n';
        CHECK(r.tail<8);CHECK(r.settle>=0 && r.settle<400);
    }
    for(bool vertical : {false,true})
    {
        const auto r=simulate(1,750,true,true,vertical,true,2'000'000);
        std::cout<<"stop/reversal "<<(vertical?'Y':'X')<<" tail="<<r.tail<<" peak="<<r.peak<<'\n';
        CHECK(r.tail<5);CHECK(r.peak<150);
    }
    auto j=std::make_shared<CommandJournal>();
    auto a=j->queued(10,5,{2,3,10},100);j->complete(a,true,100);
    CHECK(j->displacement(100,109,109).x==0);
    CHECK(j->displacement(100,110,110).x==20);
    auto b=j->queued(50,0,{},200);j->cancel(b);
    CHECK(j->displacement(110,20'000'000,200,true).x==0);
    auto d=j->queued(50,0,{},200);j->complete(d,false,200);
    CHECK(j->displacement(110,20'000'000,200,true).x==0);
    PredictiveController c(j);ControlConfig cfg;c.configure(cfg);
    CHECK(c.observe({1,100'000'000,{60,60},1},110'000'000));
    auto o=c.advance(110'000'000,{});CHECK(std::abs(o.pixels.x-o.pixels.y)<1e-10);
    CHECK(!c.advance(111'000'000,{}).due);
    CHECK(!c.observe({1,100'000'000,{600,0},1},112'000'000));
    CHECK(!c.observe({2,99'000'000,{600,0},1},112'000'000));
    CHECK(!c.advance(400'000'000,{}).valid);
    CHECK(!c.observe({3,500'000'000,{0,0},1},400'000'000));
    OutputMapper mapper;
    int sum=0;for(int i=0;i<10;++i)sum+=mapper.map({.2,0},{2,1,0},0,0)[0];
    CHECK(sum==1);
    for(int i=0;i<100;++i)CHECK(mapper.map({1000,0},{},1,1)[0]==1);
    CHECK(mapper.map({0,0},{},1,1)[0]==0);
    std::cout<<"predictive closed loop passed\n";
}
