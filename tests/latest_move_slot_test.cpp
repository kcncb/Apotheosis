#include "mouse/latest_move_slot.h"
#include <iostream>
#define CHECK(x) do { if (!(x)) { std::cerr << "failed: " #x << '\n'; return 1; } } while (0)
int main()
{
    mouse_async::LatestMoveSlot slot;
    const auto now = std::chrono::steady_clock::now();
    const auto stale = slot.replace(1, 2, now, 100, 200);
    slot.replace(3, 4, now, 300, 400);
    mouse_async::PendingMove out;
    CHECK(slot.take(out));
    CHECK(out.dx == 3 && out.dy == 4 && out.capture_ns == 300 && out.aim_ns == 400);
    CHECK(!slot.isCurrent(stale) && slot.isCurrent(out.generation));
    slot.clear();
    CHECK(!slot.isCurrent(out.generation) && !slot.take(out));
    std::cout << "move timestamp pairing: passed\n";
}
