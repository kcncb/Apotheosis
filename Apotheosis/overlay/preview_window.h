#ifndef OVERLAY_PREVIEW_WINDOW_H
#define OVERLAY_PREVIEW_WINDOW_H

// Independent OS-level detection preview window. Uses OpenCV HighGUI so it
// is decoupled from the ImGui control panel (separate HWND, free to drag and
// resize anywhere on the desktop). Lifecycle is driven by `config.show_window`:
// the worker thread polls the flag, opens an `imshow` window when on, closes
// it when off, and re-opens it if the user manually closed the OS window
// while the toggle is still enabled.
//
// Call PreviewWindow_Start() once during process bring-up after the overlay
// has initialised, and PreviewWindow_Stop() during shutdown.
void PreviewWindow_Start();
void PreviewWindow_Stop();

#endif // OVERLAY_PREVIEW_WINDOW_H
