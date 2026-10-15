#pragma once

#include <functional>
#include <vector>

// ---------------------------------------------------------------------------
// Spek-style spectrogram.
//
// This is a separate tool from the live panel: the panel borrows foobar2000's
// own FFT through visualisation_manager, which only ever exposes the moment
// around the playback cursor. To draw a whole file as time x frequency we have
// to decode it ourselves and run our own FFT.
//
// The point of the thing is the cutoff readout: lossy encoders brick-wall the
// top end (128 kbps MP3 usually stops near 16 kHz, 320 kbps near 20.5 kHz), so
// a "FLAC" whose spectrogram goes black above 16 kHz is a transcode.
// ---------------------------------------------------------------------------

//! Cells hold *level*, not colour: 0 is the floor, 255 is 0 dBFS, quantised over
//! this many dB. The window maps them through a palette using whatever dynamic
//! range the user picked, so changing that range is instant and never re-runs the
//! analysis. A wide store range costs nothing but a little precision (about
//! 0.6 dB per step here), and it is what lets quiet ultrasonic content fade out
//! gradually instead of hitting a hard black edge.
constexpr double kCellStoreRangeDb = 160.0;

//! Level of a stored cell, in dBFS.
inline double spectrogram_cell_db(unsigned char cell) {
	return (double)cell / 255.0 * kCellStoreRangeDb - kCellStoreRangeDb;
}

//! Dynamic ranges offered by the window's 动态范围 control, in dB.
unsigned spectrogram_range_count();
const char * spectrogram_range_name(unsigned index);
double spectrogram_range_value(unsigned index);

struct t_spectrogram_params {
	unsigned fftSize = 4096;   // power of two
	unsigned width   = 1600;   // output columns (time)
	unsigned height  = 512;    // output rows (frequency), row 0 = DC
	//! A frame only votes in the cutoff estimate when its peak is above this,
	//! so fade-outs and silence do not contribute noise.
	double activeFloorDb = -70.0;
	//! How far below a frame's own peak the *music* still counts as content.
	double contentDropDb = 50.0;
	//! How far below the peak any content at all still counts. This is the one
	//! that answers "does the file have a brick wall?": a lossy encoder leaves
	//! nothing above its cutoff, while real material keeps dither, noise shaping
	//! or genuine ultrasonic content running to Nyquist at a low level. Judging
	//! by contentDropDb alone calls those files lossy, which is wrong.
	double deepDropDb = 90.0;
};

struct t_spectrogram_result {
	unsigned width = 0, height = 0;
	//! width * height bytes, row 0 = DC. Index as cells[y * width + x].
	std::vector<unsigned char> cells;

	double maxFreqHz   = 22050.0; // frequency of the top row
	double durationSec = 0.0;
	double sampleRate  = 0.0;
	unsigned channels  = 0;

	//! Highest frequency that carries anything at all (deepDropDb).
	double cutoffDeepHz = 0.0;
	//! Highest frequency the music itself reaches (contentDropDb).
	double cutoffHz     = 0.0;
	//! How many columns had a frame loud enough to vote.
	unsigned columnsWithContent = 0;
	unsigned columnsTotal = 0;

	pfc::string8 note;  // free-form remark shown next to the readout
};

//! Human-readable verdict for a cutoff, e.g. "疑似 128 kbps 有损转码".
//! Deliberately hedged: a band-limited master looks the same as a transcode.
pfc::string8 spectrogram_verdict(const t_spectrogram_result & r);

//! Decodes `path` and computes the spectrogram. Blocking, so call it from a
//! worker thread; `abort` is polled throughout and makes it return false.
//! `onProgress` receives 0..1 and may be empty; it is called from this thread.
bool spectrogram_analyse(const char * path,
                         const t_spectrogram_params & params,
                         t_spectrogram_result & out,
                         abort_callback & abort,
                         const std::function<void(double)> & onProgress,
                         pfc::string8 & error);

//! Maps an intensity byte through one of the built-in palettes to 0x00RRGGBB.
unsigned spectrogram_palette_color(unsigned palette, unsigned intensity);
unsigned spectrogram_palette_count();
const char * spectrogram_palette_name(unsigned palette);

// --- view options ----------------------------------------------------------
//! Fixed upper ends offered by the frequency-axis control, in Hz. Only the ones
//! at or below the source's own Nyquist limit are ever listed: there is nothing
//! above it to draw, and offering a ceiling that cannot change anything just
//! makes the control look broken.
extern const double kSpectrogramAxisChoicesHz[];
unsigned spectrogram_axis_choice_count();

//! Dynamic ranges offered by the window's 动态范围 control, in dB.
unsigned spectrogram_range_count();
const char * spectrogram_range_name(unsigned index);
double spectrogram_range_value(unsigned index);

//! View options for the spectrogram window, remembered between sessions.
struct t_spectrogram_view {
	unsigned logAxis = 0;   // 0 = linear, 1 = logarithmic
	unsigned palette = 0;
	//! Upper end of the frequency axis in Hz; 0 means "follow the source".
	double axisMaxHz = 0.0;
	//! Index into the dynamic-range list. 3 is the 120 dB entry, which is Spek's
	//! own scale - see kRangeDb in spectrogram.cpp.
	unsigned rangeDb = 3;
};

t_spectrogram_view spectrogram_view_load();
void spectrogram_view_save(const t_spectrogram_view & v);
