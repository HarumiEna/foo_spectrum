#pragma once

// ---------------------------------------------------------------------------
// Modeless colour settings window.
//
// Opened from the spectrum panel's right-click menu. Holds everything colour
// related so the main settings window can stay free of it:
//
//   * one owner-drawn swatch per colour, showing its current hex value
//   * the two ways of picking a colour - the standard Windows picker and the
//     on-screen eyedropper - selected with a radio pair
//   * the preset list (built-in presets followed by user saved ones), with
//     apply / save-current / delete
//
// Only one instance exists at a time. The window destroys itself when closed.
// ---------------------------------------------------------------------------
void spectrum_open_color_window(HWND owner);
