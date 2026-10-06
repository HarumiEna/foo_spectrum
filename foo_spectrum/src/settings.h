#pragma once

// ---------------------------------------------------------------------------
// Settings and colour storage for the Spectrum Visualizer component.
//
// Include this *after* stdafx.h (it relies on COLORREF / Windows types).
//
// Every setting lives in foobar2000 Advanced Preferences:
//     参数设置 → 高级 → 可视化 → 频谱可视化
// The values are backed by configStore through SDK advconfig objects, so they
// persist with the rest of the player configuration automatically and can be
// edited live while the panel is running. Colours can also be changed from the
// panel's right-click menu, which opens the standard Windows colour picker.
// ---------------------------------------------------------------------------

//! Where a scale is drawn. Each axis has two sensible sides; 0 means "hidden".
enum t_scale_pos {
	scale_off = 0,
	//! Frequency: below the plot. Level: left of it.
	scale_primary = 1,
	//! Frequency: above the plot. Level: right of it.
	scale_secondary = 2,
};

struct t_spectrum_colors {
	COLORREF background = RGB(0x0A, 0x0E, 0x12);
	//! Three-stop gradient: low -> mid -> high. In vertical mode "low" is the
	//! bottom of the plot and "high" the top, which is what gives the classic
	//! green -> yellow -> red spectrum look.
	COLORREF bar_low    = RGB(0x00, 0xB0, 0x00);
	COLORREF bar_mid    = RGB(0xFF, 0xFF, 0x00);
	COLORREF bar_high   = RGB(0xFF, 0x20, 0x00);
	COLORREF peak       = RGB(0xFF, 0xFF, 0xFF);
	COLORREF grid       = RGB(0x25, 0x30, 0x3A);
	COLORREF scale      = RGB(0x8A, 0x98, 0xA8);
};

// How the bar colour varies across the display.
enum t_gradient_dir {
	//! Colour depends on the band's position on the frequency axis.
	gradient_horizontal = 0,
	//! Colour depends on the vertical position inside the plot: the classic
	//! spectrum look, low colour at the bottom, high colour at the top.
	gradient_vertical = 1,
	//! Whole bar takes one colour chosen from the bar's own level.
	gradient_by_level = 2,
};

struct t_spectrum_settings {
	unsigned bands         = 256;   // displayed band count, 64..1024
	unsigned style         = 1;     // 0 = filled bars, 1 = bars + peak hold, 2 = outline
	unsigned alpha         = 255;   // bar fill opacity, 0..255
	unsigned fps           = 60;    // refresh rate, 10..240
	unsigned gap           = 1;     // gap between bars, pixels
	unsigned peak_fall     = 60;    // peak-hold fall speed, percent of full scale per second
	unsigned bar_rise_ms   = 0;     // bar attack time constant, ms; 0 = follow the signal instantly
	unsigned bar_fall_ms   = 90;    // bar release time constant, ms; 0 = drop instantly
	unsigned gain          = 100;   // amplitude gain, percent
	unsigned gradient_mode = 0;     // 0 = three-colour gradient, 1 = rainbow sweep
	unsigned gradient_dir  = gradient_vertical; // see t_gradient_dir
	unsigned fft           = 0;     // FFT size; 0 = pick automatically from band count
	bool low_boost         = true;  // run a second, longer FFT for the low bands
	unsigned freq_min      = 20;    // lowest displayed frequency, Hz
	unsigned freq_max      = 20000; // highest displayed frequency, Hz
	bool db_scale          = true;  // map amplitude onto -60..0 dB before display
	bool log_freq          = true;  // logarithmic frequency axis
	bool grid              = false; // draw a 8x8 reference grid
	unsigned scale_freq_pos  = scale_off; // frequency scale: off / below / above
	unsigned scale_level_pos = scale_off; // level scale: off / left / right

	t_spectrum_colors colors;
};

// --- colours editable through the panel's right-click palette -------------
enum t_spectrum_color_slot {
	spectrum_color_background = 0,
	spectrum_color_bar_low,
	spectrum_color_bar_mid,
	spectrum_color_bar_high,
	spectrum_color_peak,
	spectrum_color_grid,
	spectrum_color_scale,
};
unsigned spectrum_color_slot_count();
const char * spectrum_color_slot_name(unsigned slot);
COLORREF spectrum_color_slot_get(unsigned slot);
void spectrum_color_slot_set(unsigned slot, COLORREF color);

// Reads the current value of every Advanced Preferences entry.
t_spectrum_settings spectrum_settings_load();

// Writes the whole setting set back to Advanced Preferences (values are clamped
// exactly like spectrum_settings_load() does).
void spectrum_settings_save(const t_spectrum_settings & s);

// --- configuration files (export / import) ---------------------------------
//! Serialises the current settings as UTF-8 text, suitable for a .ini style file.
pfc::string8 spectrum_settings_serialize();
//! Parses text produced by spectrum_settings_serialize() and applies it.
//! Unknown keys are ignored; out-of-range values are clamped.
//! @returns true on success, false with a UTF-8 message in `error` otherwise.
bool spectrum_settings_deserialize(const char * text, pfc::string8 & error);

// Built-in colour presets (reachable from the colour panel).
unsigned spectrum_settings_preset_count();
const char * spectrum_settings_preset_name(unsigned index);
void spectrum_settings_apply_preset(unsigned index);

// --- user colour presets ---------------------------------------------------
//! Colour sets saved by the user, kept in the component's own configuration
//! (not exposed in Advanced Preferences). Limited to 32 entries.
unsigned spectrum_user_preset_count();
//! @returns the preset's name, or "" when the index is out of range.
//! The returned pointer stays valid until the next call - copy it if needed.
const char * spectrum_user_preset_name(unsigned index);
//! Applies a user preset's colours and gradient settings.
bool spectrum_user_preset_apply(unsigned index);
//! Saves the *current* colours under `name`; an existing name is overwritten.
//! @returns false when the name is empty or the list is full.
bool spectrum_user_preset_save(const char * name);
//! Removes a user preset.
bool spectrum_user_preset_delete(unsigned index);

// Opens the foobar2000 Preferences dialog and navigates straight to this
// component's Advanced Preferences branch (foobar2000 1.5+).
void spectrum_settings_show();

// Parses "RRGGBB", "#RRGGBB", "0xRRGGBB" or the 3-digit short form "RGB".
// Returns `fallback` when the text cannot be parsed.
COLORREF spectrum_parse_color(const char * text, COLORREF fallback);
