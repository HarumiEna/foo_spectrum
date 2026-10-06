#pragma once

// ---------------------------------------------------------------------------
// Modal screen eyedropper ("取色器").
//
// Pops up a small magnifier loupe that follows the cursor. The user aims the
// crosshair at any pixel on any monitor and clicks to take that colour.
//
//   left click    accept
//   right click   cancel
//   middle click  cancel
//   ESC           cancel
//   losing mouse capture (alt-tab and friends) also cancels
//
// The loupe is offset diagonally from the cursor and flips to the other side
// near a monitor edge, so it never covers the pixel being sampled.
//
// @param owner  window that owns the loupe (it stays above it, hidden from the
//               taskbar and alt-tab)
// @param label  UTF-8 caption shown next to the hex value, may be null
// @param out    receives the picked colour when true is returned
// @returns true when a colour was picked, false when the user cancelled
// ---------------------------------------------------------------------------
bool spectrum_pick_color_from_screen(HWND owner, const char * label, COLORREF & out);
