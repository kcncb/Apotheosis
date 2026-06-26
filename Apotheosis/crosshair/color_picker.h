#ifndef CROSSHAIR_COLOR_PICKER_H
#define CROSSHAIR_COLOR_PICKER_H

#include <opencv2/core.hpp>

// Eyedropper ("取色") shared state for the colour-find palettes (crosshair,
// glass film, ...).
//
// Single process: the Qt pages, the runtime `config`, and the OpenCV
// "Detection Preview" window are all compiled into the same `ai` binary, so
// this is a plain shared-memory handoff between two threads — no IPC:
//   * a Qt UI page arms the pick and later consumes the result;
//   * the OpenCV preview thread (which also runs the mouse callback) samples
//     the clicked pixel and submits the HSV.
// All functions are thread-safe.
//
// Owner token: multiple pages (准星 / 玻璃膜) each have a 取色 button against
// the SAME single preview window. Arming returns a non-zero token; the result
// is tagged with it, and TakePickedColor only hands a result to the page whose
// token matches. So if page B arms while page A is still waiting, A sees the
// armed token change (ArmedToken) and bows out without stealing B's pick.

namespace crosshair
{

// Fixed sampling footprint: a (2*kPickHalf+1) square = 5x5 region around the
// click, sampled robustly (median S/V, circular-mean H). See SampleRegionHSV.
constexpr int kPickHalf = 2;

// Arm pick mode; returns a non-zero owner token for this session. Clears any
// stale (unconsumed) result so a fresh session never reads a leftover sample.
int  ArmColorPick();
// Force-cancel the current session (idempotent). Used by the active page's
// cancel button and by the preview's right-click — only one session is ever
// armed, so no token is needed to cancel it.
void CancelColorPick();
bool IsColorPickArmed();
// Current owner token, or 0 when idle. A page compares this to its own token
// to notice it was superseded or cancelled.
int  ArmedToken();

// Called by the preview thread once a sample has been taken: stores the HSV
// (OpenCV ranges: H 0..179, S/V 0..255), tags it with the current owner token,
// and disarms. Overwrites any prior unconsumed result.
void SubmitPickedColor(int h, int s, int v);

// Called by a Qt page to consume a pending result exactly once. Returns true
// and fills h/s/v only when a fresh sample tagged with `token` is waiting.
bool TakePickedColor(int token, int& h, int& s, int& v);

// Median-sample a square region (side 2*half+1) centred on (cx, cy) of a clean
// BGR frame and return its HSV. H uses a circular mean so reds straddling the
// 0/179 hue seam average correctly; S and V use the plain median so a few
// anti-aliased edge pixels don't drag the result. The region is clipped to the
// frame. Returns false if the frame is empty / not CV_8UC3 or the centre is
// out of bounds.
bool SampleRegionHSV(const cv::Mat& bgr, int cx, int cy, int half,
                     int& h, int& s, int& v);

} // namespace crosshair

#endif // CROSSHAIR_COLOR_PICKER_H
