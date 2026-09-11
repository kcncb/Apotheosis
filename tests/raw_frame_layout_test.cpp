#include "capture/raw_frame_layout.h"
#include <iostream>
#include <climits>
#define CHECK(x) do { if (!(x)) { std::cerr << "failed: " #x << '\n'; return 1; } } while (0)
int main()
{
    CHECK(capture::rawFrameFits(1920 * 1080 * 3 / 2, 1920, 1080, 1920, 1, true));
    CHECK(!capture::rawFrameFits(1920 * 1080, 1920, 1080, 1920, 1, true));
    CHECK(!capture::rawFrameFits(1920 * 1080 * 4, 1920, 1080, 1920, 4, false));
    CHECK(capture::rawFrameFits(1920 * 1080 * 4, 1920, 1080, -7680, 4, false));
    CHECK(!capture::rawFrameFits(1024, 32, 32, INT_MIN, 4, false));
    CHECK(!capture::rawFrameFits(4096, 33, 32, 66, 2, false));
    CHECK(!capture::rawFrameFits(4096, 32, 33, 32, 1, true));
    CHECK(capture::rawFrameFits(64 * 3 + 32, 8, 4, 64, 4, false));
    CHECK(!capture::rawFrameFits(64 * 3 + 31, 8, 4, 64, 4, false));
    std::cout << "raw sample bounds: passed\n";
}
