#ifndef MACRO_INPUT_H
#define MACRO_INPUT_H

namespace macro {

// All dispatch routes through the user-selected input backend
// (config.input_method). On Win32 fallback we use SendInput/keybd_event.
// Every call here is thread-safe — internally locks the same recursive
// mutex MouseThread does, so concurrent moves from the aim loop and from
// a macro script can't corrupt the kmbox/arduino serial framing.

// Relative cursor move in pixels.
void dispatch_move(int dx, int dy);

// Move cursor to an absolute screen coordinate (Win32 SetCursorPos under
// the hood — most hardware backends don't expose absolute positioning).
void dispatch_move_abs(int x, int y);

// button: 1=LMB, 2=RMB, 3=MMB, 4=X1, 5=X2. Out-of-range silently ignored.
void dispatch_button_down(int button);
void dispatch_button_up(int button);

// vk: Win32 VK_* code. Routes through KMBOX_NET keyDown/keyUp when
// available; otherwise falls back to keybd_event.
void dispatch_key_down(int vk);
void dispatch_key_up(int vk);

// clicks: positive = wheel up, negative = wheel down. Magnitude is in
// notch units (Logitech convention). Wheel HID emits 120 per notch — we
// scale internally so callers don't have to.
void dispatch_wheel(int clicks);

// Fired by ghub_api whenever the macro injects a button press, so the
// poller can suppress the synthetic edge it'd otherwise emit. Caller
// flags down/up; the runtime zeroes the suppression once the next poll
// observes the matching state.
void note_synthetic_button(int button, bool pressed);

} // namespace macro

#endif // MACRO_INPUT_H
