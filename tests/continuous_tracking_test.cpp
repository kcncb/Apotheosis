#include "tracking_scenarios.h"
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
int main()
{
    // Budgets apply to the full motion interval; relaxing a settle-tail test
    // cannot hide a regression here. Units are the simulated crop's pixels.
    constexpr double p95_budget[]={1.8,5,7,8,4.8,11.5,4.5,.01,25};
    constexpr double peak_budget[]={2.3,6.5,9,18.5,6.8,15,5.5,.01,27};
    constexpr double tight_budget[]={99,70,80,85,75,70,65,100,85};
    for(int scenario=0;scenario<9;++scenario)
    {
        const auto m=tracking_test::simulate(scenario);
        std::printf("%s: p95=%.3f peak=%.3f within3=%.1f%%\n",tracking_test::kNames[scenario],m.p95,m.peak,m.within_three_percent);
        CHECK(m.p95<=p95_budget[scenario]);CHECK(m.peak<=peak_budget[scenario]);CHECK(m.within_three_percent>=tight_budget[scenario]);
        if(scenario==7) CHECK(m.commands==0);
    }
    const tracking_test::Environment variants[]={
        {240,16,1,120,.2,false,1}, {120,32,1,120,.2,true,1}, {120,48,1,120,1,true,1},
        {120,16,.5,120,.2,true,.5}, {120,16,2,120,.2,true,2}, {120,16,1.2,120,.2,true,1},
        {120,16,1,60,1,true,1}, {240,12,1,240,.2,true,1}, {120,16,1,120,2,false,1},
        {120,16,1,120,.2,true,1,2}, {120,16,1,120,.2,true,1,-2}, {120,16,1,120,.2,true,1,0,2.5}};
    for(const auto& env:variants) for(int scenario=0;scenario<9;++scenario)
    {
        const auto m=tracking_test::simulate(scenario,env);
        CHECK(std::isfinite(m.peak));CHECK(m.peak<180);
        if(scenario==7) CHECK(m.peak<6);
    }
    // A command near a capture boundary is uncertain; failed/cancelled/queued
    // commands cannot create fake executed-motion evidence.
    motion::CommandJournal journal;
    auto id=journal.queued(20,10,{1,1,5'000'000,2'000'000},100'000'000);
    CHECK(journal.timingVariance(100'000'000,106'000'000).x==0);
    journal.complete(id,true,100'000'000);
    CHECK(journal.timingVariance(100'000'000,106'000'000).x==100);
    CHECK(journal.timingVariance(110'000'000,120'000'000).x==0);
    auto cancelled=journal.queued(100,100,{},110'000'000);journal.cancel(cancelled);
    CHECK(journal.timingVariance(116'000'000,120'000'000).x==0);
    std::puts("continuous motion regressions passed");
}
