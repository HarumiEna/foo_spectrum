#pragma once

// ---------------------------------------------------------------------------
// Modeless settings window for the Spectrum Visualizer.
//
// Opened from the panel's right-click menu. Every numeric/boolean/combo field
// carries a short label plus a tooltip with the full explanation, and the
// colour buttons open the standard Windows colour picker.
//
// Only one instance exists at a time: calling this again brings the existing
// window to the front. The window destroys itself when closed.
// ---------------------------------------------------------------------------
void spectrum_open_settings_window(HWND owner);
