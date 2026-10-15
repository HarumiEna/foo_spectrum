#pragma once

// ---------------------------------------------------------------------------
// Modeless spectrogram window (Spek-style).
//
// Opened from the spectrum panel's right-click menu. Analyses the track that is
// currently playing, or a file chosen with the standard open dialog, and shows
// the whole thing as a time x frequency heat map with an estimated cutoff
// frequency - the number you look at to decide whether a file is really
// lossless or a lossy transcode.
//
// Only one instance exists at a time. The window destroys itself when closed.
// ---------------------------------------------------------------------------
void spectrum_open_spectrogram_window(HWND owner);
