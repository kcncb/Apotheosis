#ifndef CROSSHAIR_COLOR_PICKER_H
#define CROSSHAIR_COLOR_PICKER_H

#include <opencv2/core.hpp>

// Eyedropper ("取色") shared state for the crosshair colour-find palette.
//
// Single process: the Qt CrosshairPage, the runtime `config`, and the OpenCV
// "Detection Preview" window are all compiled into the same `ai` binary, so
// this is a plain shared-memory handoff between two threads — no IPC:
//   * the Qt UI thread arms the pick and later consumes the result;
//   * the OpenCV preview thread (which also runs the mouse callback) samples
//     the clicked pixel and submits the HSV.
// All functions are thread-safe.

namespace crosshair
{

// Fixed sampling footprint: a (2*kPickHalf+1) square = 5x5 region around the
// click, sampled robustly (median S/V, circular-mean H). See SampleRegionHSV.
constexpr int kPickHalf = 2;

// Arm / disarm pick mode. Arming clears any stale (unconsumed) result so a
// fresh session never reads a leftover sample.
void ArmColorPick();
void CancelColorPick();
bool IsColorPickArmed();

// Called by the preview thread once a sample has been taken: stores the HSV
// (OpenCV ranges: H 0..179, S/V 0..255) and disarms. Overwrites any prior
// unconsumed result.
void SubmitPickedColor(int h, int s, int v);

// Called by the Qt thread to consume a pending result exactly once. Returns
// true and fills h/s/v when a fresh sample is waiting, false otherwise.
bool TakePickedColor(int& h, int& s, int& v);

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
