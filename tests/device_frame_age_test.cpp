#include "capture/device_frame_age.h"
#include <iostream>
#include <cstdlib>
#define CHECK(x) do { if (!(x)) { std::cerr << "failed: " #x << '\n'; return EXIT_FAILURE; } } while (0)
int main()
{
    capture::DeviceFrameAge age;
    constexpr int64_t now = 10'000'000'000;
    constexpr uint64_t qpc = 1'000'000'000;
    constexpr double file = 133'000'000'000'000'000.0;
    CHECK(age.read(now) == -1);
    age.update(qpc - 100'000, qpc, file, now);
    CHECK(age.read(now) == 10000);
    CHECK(age.read(now + 2'000'000'001) == -1);
    age.update(0, qpc, file, now + 1);
    CHECK(age.read(now + 1) == -1);
    age.update(qpc, qpc, file, now + 2);
    CHECK(age.read(now + 2) == 0);
    age.update(1, qpc + 10'000'000'000.0, file, now + 3);
    CHECK(age.read(now + 3) == -1);
    // A real one-second driver stall must remain visible (old cutoff was 500ms).
    age.update(qpc - 10'000'000, qpc, file, now + 4);
    CHECK(age.read(now + 4) == 1'000'000);
    age.invalidate();
    age.update(static_cast<uint64_t>(file - 200'000), qpc, file, now + 5);
    CHECK(age.read(now + 5) >= 19990 && age.read(now + 5) <= 20010);
    // Invalidating a changed epoch allows the next sample to establish it again.
    age.update(qpc, qpc, file, now + 6);
    CHECK(age.read(now + 6) == -1);
    age.update(qpc, qpc, file, now + 7);
    CHECK(age.read(now + 7) == 0);
    std::cout << "device frame age: passed\n";
}
