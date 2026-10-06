#include "stdafx.h"
#include "settings.h"

// ---------------------------------------------------------------------------
// 高级参数注册。
//
// 下面的 GUID 均属于本组件，既作为配置存储键，也作为高级参数项标识，必须唯一。
//
// 每个条目的第一个参数是显示名称（已汉化，UTF-8），第二个参数是
// configStore 存储键 —— 存储键必须保持 ASCII 且永不改动，否则用户配置会丢失。
// ---------------------------------------------------------------------------
namespace {

	static constexpr GUID guid_branch      = { 0x1b7e4a52, 0x6c3d, 0x4f80, { 0xa1, 0xb2, 0x3c, 0x4d, 0x5e, 0x6f, 0x70, 0x81 } };
	static constexpr GUID guid_bands       = { 0x2c8f5b63, 0x7d4e, 0x4091, { 0xb2, 0xc3, 0x4d, 0x5e, 0x6f, 0x70, 0x81, 0x92 } };
	static constexpr GUID guid_style       = { 0x3d906c74, 0x8e5f, 0x41a2, { 0xc3, 0xd4, 0x5e, 0x6f, 0x70, 0x81, 0x92, 0xa3 } };
	static constexpr GUID guid_alpha       = { 0x4ea17d85, 0x9f60, 0x42b3, { 0xd4, 0xe5, 0x6f, 0x70, 0x81, 0x92, 0xa3, 0xb4 } };
	static constexpr GUID guid_fps         = { 0x5fb28e96, 0xa071, 0x43c4, { 0xe5, 0xf6, 0x70, 0x81, 0x92, 0xa3, 0xb4, 0xc5 } };
	static constexpr GUID guid_gap         = { 0x60c39fa7, 0xb182, 0x44d5, { 0xf6, 0x07, 0x81, 0x92, 0xa3, 0xb4, 0xc5, 0xd6 } };
	static constexpr GUID guid_peakfall    = { 0x71d4a0b8, 0xc293, 0x45e6, { 0x07, 0x18, 0x92, 0xa3, 0xb4, 0xc5, 0xd6, 0xe7 } };
	// 注意：这两个 GUID 以前和 guid_dbscale / guid_logfreq 是同一组值（重复），
	// 已改成 000d / 000e。存储用的是下面的 ASCII 键名，所以值不会丢。
	static constexpr GUID guid_bar_rise    = { 0xa1b2c3d4, 0x000d, 0x4a01, { 0x90, 0x0d, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 } };
	static constexpr GUID guid_bar_fall    = { 0xa1b2c3d4, 0x000e, 0x4a01, { 0x90, 0x0e, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 } };
	static constexpr GUID guid_gain        = { 0x82e5b1c9, 0xd3a4, 0x46f7, { 0x18, 0x29, 0xa3, 0xb4, 0xc5, 0xd6, 0xe7, 0xf8 } };
	static constexpr GUID guid_slope       = { 0xa1b2c3d4, 0x000c, 0x4a01, { 0x90, 0x0c, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 } };
	static constexpr GUID guid_dbscale     = { 0x93f6c2da, 0xe4b5, 0x4708, { 0x29, 0x3a, 0xb4, 0xc5, 0xd6, 0xe7, 0xf8, 0x09 } };
	static constexpr GUID guid_logfreq     = { 0xa407d3eb, 0xf5c6, 0x4819, { 0x3a, 0x4b, 0xc5, 0xd6, 0xe7, 0xf8, 0x09, 0x1a } };
	static constexpr GUID guid_grid        = { 0xb518e4fc, 0x06d7, 0x492a, { 0x4b, 0x5c, 0xd6, 0xe7, 0xf8, 0x09, 0x1a, 0x2b } };
	static constexpr GUID guid_gradmode    = { 0xd73a061e, 0x28f9, 0x4b4c, { 0x6d, 0x7e, 0xf8, 0x09, 0x1a, 0x2b, 0x3c, 0x4d } };
	static constexpr GUID guid_graddir     = { 0xa1b2c3d4, 0x0005, 0x4a01, { 0x90, 0x05, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 } };
	static constexpr GUID guid_col_bg      = { 0xe84b172f, 0x390a, 0x4c5d, { 0x7e, 0x8f, 0x09, 0x1a, 0x2b, 0x3c, 0x4d, 0x5e } };
	static constexpr GUID guid_col_low     = { 0xf95c2840, 0x4a1b, 0x4d6e, { 0x8f, 0x90, 0x1a, 0x2b, 0x3c, 0x4d, 0x5e, 0x6f } };
	static constexpr GUID guid_col_mid     = { 0xa1b2c3d4, 0x0006, 0x4a01, { 0x90, 0x06, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 } };
	static constexpr GUID guid_col_high    = { 0x0a6d3951, 0x5b2c, 0x4e7f, { 0x90, 0xa1, 0x2b, 0x3c, 0x4d, 0x5e, 0x6f, 0x70 } };
	static constexpr GUID guid_col_peak    = { 0x1b7e4a62, 0x6c3d, 0x4f80, { 0xa1, 0xb2, 0x3c, 0x4d, 0x5e, 0x6f, 0x70, 0x81 } };
	static constexpr GUID guid_col_grid    = { 0x2c8f5b73, 0x7d4e, 0x4091, { 0xb2, 0xc3, 0x4d, 0x5e, 0x6f, 0x70, 0x81, 0x92 } };
	static constexpr GUID guid_col_scale   = { 0xa1b2c3d4, 0x0003, 0x4a01, { 0x90, 0x03, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 } };
	static constexpr GUID guid_scale_freq  = { 0xa1b2c3d4, 0x0001, 0x4a01, { 0x90, 0x01, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 } };
	static constexpr GUID guid_scale_level = { 0xa1b2c3d4, 0x0002, 0x4a01, { 0x90, 0x02, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 } };
	static constexpr GUID guid_fft         = { 0xa1b2c3d4, 0x0004, 0x4a01, { 0x90, 0x04, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 } };
	static constexpr GUID guid_lowboost    = { 0xa1b2c3d4, 0x0007, 0x4a01, { 0x90, 0x07, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 } };
	static constexpr GUID guid_freq_min    = { 0xa1b2c3d4, 0x0009, 0x4a01, { 0x90, 0x09, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 } };
	static constexpr GUID guid_freq_max    = { 0xa1b2c3d4, 0x000a, 0x4a01, { 0x90, 0x0a, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 } };
	static constexpr GUID guid_userpresets = { 0xa1b2c3d4, 0x000b, 0x4a01, { 0x90, 0x0b, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 } };

	// 该分支会出现在高级参数页的「可视化」节点下。
	static advconfig_branch_factory g_branch(
		"频谱可视化 (Spectrum Visualizer)", guid_branch, advconfig_entry::guid_branch_vis, 0.0);

	static advconfig_integer_factory g_bands(
		"波段数量", "foo_spectrum.bands",
		guid_bands, guid_branch, 0.0, 256, 64, 1024);

	static advconfig_integer_factory g_style(
		"显示样式", "foo_spectrum.style",
		guid_style, guid_branch, 0.1, 1, 0, 2);

	static advconfig_integer_factory g_alpha(
		"不透明度", "foo_spectrum.alpha",
		guid_alpha, guid_branch, 0.2, 255, 0, 255);

	static advconfig_integer_factory g_fps(
		"刷新率 (FPS)", "foo_spectrum.fps",
		guid_fps, guid_branch, 0.3, 60, 10, 240);

	static advconfig_integer_factory g_gap(
		"柱间空隙 (px)", "foo_spectrum.gap",
		guid_gap, guid_branch, 0.4, 1, 0, 8);

	static advconfig_integer_factory g_peakfall(
		"峰值下落速度 (%/s)", "foo_spectrum.peakfall",
		guid_peakfall, guid_branch, 0.5, 60, 0, 400);

	// --- 柱体动态 -----------------------------------------------------------
	static advconfig_integer_factory g_bar_rise(
		"柱子上升时间 (ms)", "foo_spectrum.bar.rise",
		guid_bar_rise, guid_branch, 0.55, 0, 0, 500);

	static advconfig_integer_factory g_bar_fall(
		"柱子下落时间 (ms)", "foo_spectrum.bar.fall",
		guid_bar_fall, guid_branch, 0.56, 90, 0, 2000);

	static advconfig_integer_factory g_gain(
		"增益 (%)", "foo_spectrum.gain",
		guid_gain, guid_branch, 0.6, 100, 1, 400);

	// 频谱倾斜：以 1 kHz 为轴心，每倍频程抬高（正）或压低（负）多少 dB。
	// 音乐自然衰减约 6 dB/倍频程，所以 +4～+6 就能把整条曲线掰平。
	// 用有符号版本，负值可以反过来强调低频。
	static advconfig_signed_integer_factory g_slope(
		"频谱倾斜 (dB/oct)", "foo_spectrum.slope",
		guid_slope, guid_branch, 0.65, 0, -12, 12);

	static advconfig_checkbox_factory g_dbscale(
		"dB 刻度", "foo_spectrum.dbscale",
		guid_dbscale, guid_branch, 0.7, true);

	static advconfig_checkbox_factory g_logfreq(
		"对数频率轴", "foo_spectrum.logfreq",
		guid_logfreq, guid_branch, 0.8, true);

	static advconfig_checkbox_factory g_grid(
		"显示网格", "foo_spectrum.grid",
		guid_grid, guid_branch, 0.9, false);

	static advconfig_integer_factory g_gradmode(
		"渐变模式", "foo_spectrum.gradmode",
		guid_gradmode, guid_branch, 1.0, 0, 0, 1);

	static advconfig_integer_factory g_graddir(
		"渐变方向", "foo_spectrum.grad.dir",
		guid_graddir, guid_branch, 1.1, 1, 0, 2);

	// --- 刻度 ---------------------------------------------------------------
	// 0 = 不显示，1 = 主侧（频率在下方 / 电平在左侧），2 = 副侧（上方 / 右侧）。
	// 键名沿用以前的复选框键：旧配置里存的是 0/1，正好对应「不显示 / 主侧」。
	static advconfig_integer_factory g_scale_freq(
		"频率刻度位置", "foo_spectrum.scale.freq",
		guid_scale_freq, guid_branch, 1.2, 0, 0, 2);

	static advconfig_integer_factory g_scale_level(
		"电平刻度位置", "foo_spectrum.scale.level",
		guid_scale_level, guid_branch, 1.3, 0, 0, 2);

	// --- 频率分辨率 ---------------------------------------------------------
	static advconfig_integer_factory g_fft(
		"FFT 点数", "foo_spectrum.fft",
		guid_fft, guid_branch, 1.4, 0, 0, 16384);

	static advconfig_checkbox_factory g_lowboost(
		"低频增强", "foo_spectrum.lowboost",
		guid_lowboost, guid_branch, 1.5, true);

	// --- 显示频率范围 -------------------------------------------------------
	// 只显示这一段频率，其余频段直接不画（等价于把该段放大铺满面板）。
	static advconfig_integer_factory g_freq_min(
		"显示频率下限 (Hz)", "foo_spectrum.freq.min",
		guid_freq_min, guid_branch, 1.6, 20, 20, 20000);

	static advconfig_integer_factory g_freq_max(
		"显示频率上限 (Hz)", "foo_spectrum.freq.max",
		guid_freq_max, guid_branch, 1.7, 20000, 20, 20000);

	// 用户自定义配色预设，存在组件自己的配置里，不出现在高级参数列表中。
	static cfg_string g_userPresets(guid_userpresets, "");

	// --- 颜色 ---------------------------------------------------------------
	// 以文本形式存储，便于在高级参数中直接阅读与手工修改；
	// 面板右键菜单与设置面板会把它们接到 Windows 标准取色器上。
	static advconfig_string_factory g_col_bg(
		"背景色", "foo_spectrum.col.bg",
		guid_col_bg, guid_branch, 2.0, "0A0E12");

	static advconfig_string_factory g_col_low(
		"渐变底部色", "foo_spectrum.col.low",
		guid_col_low, guid_branch, 2.1, "00B000");

	static advconfig_string_factory g_col_mid(
		"渐变中间色", "foo_spectrum.col.mid",
		guid_col_mid, guid_branch, 2.2, "FFFF00");

	static advconfig_string_factory g_col_high(
		"渐变顶部色", "foo_spectrum.col.high",
		guid_col_high, guid_branch, 2.3, "FF2000");

	static advconfig_string_factory g_col_peak(
		"峰值颜色", "foo_spectrum.col.peak",
		guid_col_peak, guid_branch, 2.4, "FFFFFF");

	static advconfig_string_factory g_col_grid(
		"网格颜色", "foo_spectrum.col.grid",
		guid_col_grid, guid_branch, 2.5, "25303A");

	static advconfig_string_factory g_col_scale(
		"刻度颜色", "foo_spectrum.col.scale",
		guid_col_scale, guid_branch, 2.6, "8A98A8");

	// -----------------------------------------------------------------------
	// 配色预设
	// -----------------------------------------------------------------------
	struct t_color_preset {
		const char * name;
		const char * bg;
		const char * low;
		const char * mid;
		const char * high;
		const char * peak;
		const char * grid;
		const char * scale;
		unsigned gradient_mode;
		unsigned gradient_dir;
	};

	static const t_color_preset g_presets[] = {
		// 经典频谱：下绿 → 中黄 → 上红
		{ "经典绿",     "000000", "00B000", "FFFF00", "FF2000", "FFFFFF", "0A2A0A", "4A9A4A", 0, 1 },
		{ "深海蓝",     "02060F", "00324D", "20A0D0", "38C8FF", "E0F7FF", "0A2030", "3A7A9A", 0, 1 },
		{ "落日火焰",   "100404", "600000", "FF8000", "FFD24A", "FFFFFF", "301010", "9A6A3A", 0, 1 },
		{ "彩虹",       "101014", "4040FF", "40FF40", "FF3030", "FFFFFF", "202028", "909090", 1, 1 },
		{ "霓虹紫",     "0A0212", "3B0060", "C000FF", "FF00FF", "FFE0FF", "1A0A2A", "9A6AAA", 0, 1 },
		{ "琥珀示波器", "100C00", "4A3000", "C08000", "FFB000", "FFF0C0", "2A1E00", "9A7A3A", 0, 1 },
		{ "黑白",       "000000", "303030", "A0A0A0", "FFFFFF", "FFFFFF", "202020", "808080", 0, 1 },
	};

	// --- 辅助函数 -----------------------------------------------------------

	inline int hex_digit(char c) {
		if (c >= '0' && c <= '9') return c - '0';
		if (c >= 'a' && c <= 'f') return c - 'a' + 10;
		if (c >= 'A' && c <= 'F') return c - 'A' + 10;
		return -1;
	}

	COLORREF parse_color(const char * text, COLORREF fallback) {
		if (text == nullptr) return fallback;
		while (*text == ' ' || *text == '\t') ++text;
		if (*text == '#') ++text;
		if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text += 2;

		int v[6];
		int n = 0;
		for (; n < 6; ++n) {
			const int h = hex_digit(text[n]);
			if (h < 0) break;
			v[n] = h;
		}
		if (n >= 6) {
			return RGB(v[0] * 16 + v[1], v[2] * 16 + v[3], v[4] * 16 + v[5]);
		}
		if (n == 3) {
			// #RGB 缩写 -> #RRGGBB
			return RGB(v[0] * 17, v[1] * 17, v[2] * 17);
		}
		return fallback;
	}

	COLORREF read_color(advconfig_string_factory & entry, COLORREF fallback) {
		pfc::string8 text;
		entry.get(text);
		return parse_color(text.c_str(), fallback);
	}

	void write_color(advconfig_string_factory & entry, COLORREF c) {
		char buf[16];
		wsprintfA(buf, "%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
		entry.set(buf);
	}

	//! Maps a palette slot to the advconfig entry holding it.
	advconfig_string_factory * color_slot_entry(unsigned slot) {
		switch (slot) {
		case spectrum_color_background: return &g_col_bg;
		case spectrum_color_bar_low:    return &g_col_low;
		case spectrum_color_bar_mid:    return &g_col_mid;
		case spectrum_color_bar_high:   return &g_col_high;
		case spectrum_color_peak:       return &g_col_peak;
		case spectrum_color_grid:       return &g_col_grid;
		case spectrum_color_scale:      return &g_col_scale;
		}
		return nullptr;
	}

} // 匿名命名空间

// ---------------------------------------------------------------------------
// 对外接口
// ---------------------------------------------------------------------------

COLORREF spectrum_parse_color(const char * text, COLORREF fallback) {
	return parse_color(text, fallback);
}

unsigned spectrum_color_slot_count() {
	return 7;
}

const char * spectrum_color_slot_name(unsigned slot) {
	switch (slot) {
	case spectrum_color_background: return "背景色";
	case spectrum_color_bar_low:    return "渐变底部颜色";
	case spectrum_color_bar_mid:    return "渐变中间颜色";
	case spectrum_color_bar_high:   return "渐变顶部颜色";
	case spectrum_color_peak:       return "峰值标记颜色";
	case spectrum_color_grid:       return "网格颜色";
	case spectrum_color_scale:      return "刻度颜色";
	}
	return "";
}

COLORREF spectrum_color_slot_get(unsigned slot) {
	advconfig_string_factory * e = color_slot_entry(slot);
	if (e == nullptr) return RGB(0, 0, 0);

	static const COLORREF fallback[7] = {
		RGB(0x0A, 0x0E, 0x12), RGB(0x00, 0xB0, 0x00), RGB(0xFF, 0xFF, 0x00),
		RGB(0xFF, 0x20, 0x00), RGB(0xFF, 0xFF, 0xFF), RGB(0x25, 0x30, 0x3A),
		RGB(0x8A, 0x98, 0xA8),
	};
	return read_color(*e, fallback[slot < 7 ? slot : 0]);
}

void spectrum_color_slot_set(unsigned slot, COLORREF color) {
	advconfig_string_factory * e = color_slot_entry(slot);
	if (e == nullptr) return;
	write_color(*e, color);
}

// Applies the same validation to values coming from configStore and from an
// imported configuration file.
static void normalize_settings(t_spectrum_settings & s) {
	if (s.bands < 64) s.bands = 64;
	if (s.bands > 1024) s.bands = 1024;
	if (s.style > 2) s.style = 2;
	if (s.alpha > 255) s.alpha = 255;
	if (s.fps < 10) s.fps = 10;
	if (s.fps > 240) s.fps = 240;
	if (s.gap > 8) s.gap = 8;
	if (s.peak_fall > 400) s.peak_fall = 400;
	if (s.bar_rise_ms > 500) s.bar_rise_ms = 500;
	if (s.bar_fall_ms > 2000) s.bar_fall_ms = 2000;
	if (s.scale_freq_pos > 2) s.scale_freq_pos = 2;
	if (s.scale_level_pos > 2) s.scale_level_pos = 2;
	if (s.gain < 1) s.gain = 1;
	if (s.gain > 400) s.gain = 400;
	if (s.slope_db_per_oct < -12) s.slope_db_per_oct = -12;
	if (s.slope_db_per_oct > 12) s.slope_db_per_oct = 12;
	if (s.gradient_mode > 1) s.gradient_mode = 1;
	if (s.gradient_dir > 2) s.gradient_dir = 2;
	// 0 = auto; anything else must be a power of two within the supported range.
	if (s.fft != 0 && (s.fft < 2048 || s.fft > 16384 || (s.fft & (s.fft - 1)) != 0)) s.fft = 0;
	if (s.freq_min < 20) s.freq_min = 20;
	if (s.freq_min > 20000) s.freq_min = 20000;
	if (s.freq_max < 20) s.freq_max = 20;
	if (s.freq_max > 20000) s.freq_max = 20000;
	if (s.freq_max <= s.freq_min) {
		// A collapsed range would show nothing at all - fall back to the full band.
		s.freq_min = 20;
		s.freq_max = 20000;
	}
}

t_spectrum_settings spectrum_settings_load() {
	t_spectrum_settings s;

	s.bands         = (unsigned)g_bands.get();
	s.style         = (unsigned)g_style.get();
	s.alpha         = (unsigned)g_alpha.get();
	s.fps           = (unsigned)g_fps.get();
	s.gap           = (unsigned)g_gap.get();
	s.peak_fall     = (unsigned)g_peakfall.get();
	s.bar_rise_ms   = (unsigned)g_bar_rise.get();
	s.bar_fall_ms   = (unsigned)g_bar_fall.get();
	s.gain          = (unsigned)g_gain.get();
	s.slope_db_per_oct = (int)g_slope.get();
	s.gradient_mode = (unsigned)g_gradmode.get();
	s.gradient_dir  = (unsigned)g_graddir.get();
	s.fft           = (unsigned)g_fft.get();
	s.low_boost     = g_lowboost.get();
	s.freq_min      = (unsigned)g_freq_min.get();
	s.freq_max      = (unsigned)g_freq_max.get();
	s.db_scale      = g_dbscale.get();
	s.log_freq      = g_logfreq.get();
	s.grid          = g_grid.get();
	s.scale_freq_pos  = (unsigned)g_scale_freq.get();
	s.scale_level_pos = (unsigned)g_scale_level.get();

	s.colors.background = read_color(g_col_bg,    RGB(0x0A, 0x0E, 0x12));
	s.colors.bar_low    = read_color(g_col_low,   RGB(0x00, 0xB0, 0x00));
	s.colors.bar_mid    = read_color(g_col_mid,   RGB(0xFF, 0xFF, 0x00));
	s.colors.bar_high   = read_color(g_col_high,  RGB(0xFF, 0x20, 0x00));
	s.colors.peak       = read_color(g_col_peak,  RGB(0xFF, 0xFF, 0xFF));
	s.colors.grid       = read_color(g_col_grid,  RGB(0x25, 0x30, 0x3A));
	s.colors.scale      = read_color(g_col_scale, RGB(0x8A, 0x98, 0xA8));

	normalize_settings(s);
	return s;
}

void spectrum_settings_save(const t_spectrum_settings & in) {
	t_spectrum_settings s = in;
	normalize_settings(s);

	g_bands.set(s.bands);
	g_style.set(s.style);
	g_alpha.set(s.alpha);
	g_fps.set(s.fps);
	g_gap.set(s.gap);
	g_peakfall.set(s.peak_fall);
	g_bar_rise.set(s.bar_rise_ms);
	g_bar_fall.set(s.bar_fall_ms);
	g_gain.set(s.gain);
	g_slope.set(s.slope_db_per_oct);
	g_gradmode.set(s.gradient_mode);
	g_graddir.set(s.gradient_dir);
	g_fft.set(s.fft);
	g_lowboost.set(s.low_boost);
	g_freq_min.set(s.freq_min);
	g_freq_max.set(s.freq_max);
	g_dbscale.set(s.db_scale);
	g_logfreq.set(s.log_freq);
	g_grid.set(s.grid);
	g_scale_freq.set(s.scale_freq_pos);
	g_scale_level.set(s.scale_level_pos);

	write_color(g_col_bg,    s.colors.background);
	write_color(g_col_low,   s.colors.bar_low);
	write_color(g_col_mid,   s.colors.bar_mid);
	write_color(g_col_high,  s.colors.bar_high);
	write_color(g_col_peak,  s.colors.peak);
	write_color(g_col_grid,  s.colors.grid);
	write_color(g_col_scale, s.colors.scale);
}

unsigned spectrum_settings_preset_count() {
	return (unsigned)(sizeof(g_presets) / sizeof(g_presets[0]));
}

const char * spectrum_settings_preset_name(unsigned index) {
	if (index >= spectrum_settings_preset_count()) return "";
	return g_presets[index].name;
}

void spectrum_settings_apply_preset(unsigned index) {
	if (index >= spectrum_settings_preset_count()) return;
	const t_color_preset & p = g_presets[index];

	g_col_bg.set(p.bg);
	g_col_low.set(p.low);
	g_col_mid.set(p.mid);
	g_col_high.set(p.high);
	g_col_peak.set(p.peak);
	g_col_grid.set(p.grid);
	g_col_scale.set(p.scale);
	g_gradmode.set(p.gradient_mode);
	g_graddir.set(p.gradient_dir);
}

void spectrum_settings_show() {
	// 传入 advconfig 分支的 GUID，foobar2000 会直接把「高级参数」定位到该分支。
	auto ui = ui_control::get();
	if (ui.is_valid()) ui->show_preferences(guid_branch);
}

// ---------------------------------------------------------------------------
// 配置文件导出 / 导入
//
// 格式是刻意做得最朴素的 key=value 文本：既能被本组件读回，也能手改、
// 能进版本管理、能贴到论坛上分享。解析全部手写，不依赖 CRT 的 strchr/atoi。
// ---------------------------------------------------------------------------
namespace {

	bool key_equals(const char * a, const char * b) {
		for (;; ++a, ++b) {
			char ca = *a, cb = *b;
			if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
			if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
			if (ca != cb) return false;
			if (ca == 0) return true;
		}
	}

	int parse_int(const char * s) {
		int v = 0;
		bool neg = false;
		if (*s == '-') { neg = true; ++s; }
		while (*s >= '0' && *s <= '9') {
			if (v < 100000000) v = v * 10 + (*s - '0');
			++s;
		}
		return neg ? -v : v;
	}

	void trim_inplace(pfc::string8 & s) {
		const char * b = s.get_ptr();
		const char * e = b + s.length();
		while (b < e && (*b == ' ' || *b == '\t')) ++b;
		while (e > b && (e[-1] == ' ' || e[-1] == '\t')) --e;
		s.set_string(b, (t_size)(e - b));
	}

	void append_num(pfc::string8 & out, const char * key, int value) {
		char buf[64];
		wsprintfA(buf, "%s=%d\n", key, value);
		out += buf;
	}

	pfc::string8 color_to_text(COLORREF c) {
		char buf[16];
		wsprintfA(buf, "%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
		return pfc::string8(buf);
	}

	void append_color(pfc::string8 & out, const char * key, COLORREF c) {
		out += key;
		out += "=";
		out += color_to_text(c);
		out += "\n";
	}

} // 匿名命名空间

pfc::string8 spectrum_settings_serialize() {
	const t_spectrum_settings s = spectrum_settings_load();

	pfc::string8 out;
	out += "; foo_spectrum - 频谱可视化 配置文件\n"
	       "; 可直接编辑，然后用设置面板里的「导入配置...」读回。\n"
	       "; 颜色为 RRGGBB 十六进制；fft=0 表示自动（按波段数决定）。\n";

	append_num(out, "version", 1);
	append_num(out, "bands", (int)s.bands);
	append_num(out, "style", (int)s.style);
	append_num(out, "alpha", (int)s.alpha);
	append_num(out, "fps", (int)s.fps);
	append_num(out, "gap", (int)s.gap);
	append_num(out, "peak_fall", (int)s.peak_fall);
	append_num(out, "bar_rise_ms", (int)s.bar_rise_ms);
	append_num(out, "bar_fall_ms", (int)s.bar_fall_ms);
	append_num(out, "gain", (int)s.gain);
	append_num(out, "slope", s.slope_db_per_oct);
	append_num(out, "gradient_mode", (int)s.gradient_mode);
	append_num(out, "gradient_dir", (int)s.gradient_dir);
	append_num(out, "fft", (int)s.fft);
	append_num(out, "low_boost", s.low_boost ? 1 : 0);
	append_num(out, "freq_min", (int)s.freq_min);
	append_num(out, "freq_max", (int)s.freq_max);
	append_num(out, "db_scale", s.db_scale ? 1 : 0);
	append_num(out, "log_freq", s.log_freq ? 1 : 0);
	append_num(out, "grid", s.grid ? 1 : 0);
	append_num(out, "scale_freq", (int)s.scale_freq_pos);
	append_num(out, "scale_level", (int)s.scale_level_pos);

	append_color(out, "color.background", s.colors.background);
	append_color(out, "color.bar_low",    s.colors.bar_low);
	append_color(out, "color.bar_mid",    s.colors.bar_mid);
	append_color(out, "color.bar_high",   s.colors.bar_high);
	append_color(out, "color.peak",       s.colors.peak);
	append_color(out, "color.grid",       s.colors.grid);
	append_color(out, "color.scale",      s.colors.scale);

	return out;
}

bool spectrum_settings_deserialize(const char * text, pfc::string8 & error) {
	if (text == nullptr || *text == 0) {
		error = "文件是空的";
		return false;
	}

	t_spectrum_settings s = spectrum_settings_load(); // unknown keys keep current values
	bool sawAny = false;

	const char * p = text;
	while (*p != 0) {
		const char * e = p;
		while (*e != 0 && *e != '\n' && *e != '\r') ++e;

		pfc::string8 line;
		line.set_string(p, (t_size)(e - p));
		p = e;
		while (*p == '\r' || *p == '\n') ++p;

		trim_inplace(line);
		const char * raw = line.get_ptr();
		if (*raw == 0 || *raw == ';' || *raw == '#') continue;

		const char * eq = nullptr;
		for (const char * q = raw; *q != 0; ++q) {
			if (*q == '=') { eq = q; break; }
		}
		if (eq == nullptr) continue;

		pfc::string8 key, val;
		key.set_string(raw, (t_size)(eq - raw));
		val.set_string(eq + 1);
		trim_inplace(key);
		trim_inplace(val);
		if (key.is_empty()) continue;

		int num = parse_int(val.get_ptr());
		if (num < 0) num = 0;
		const char * k = key.get_ptr();

		if (key_equals(k, "bands"))              { s.bands = (unsigned)num; sawAny = true; }
		else if (key_equals(k, "style"))         { s.style = (unsigned)num; sawAny = true; }
		else if (key_equals(k, "alpha"))         { s.alpha = (unsigned)num; sawAny = true; }
		else if (key_equals(k, "fps"))           { s.fps = (unsigned)num; sawAny = true; }
		else if (key_equals(k, "gap"))           { s.gap = (unsigned)num; sawAny = true; }
		else if (key_equals(k, "peak_fall"))     { s.peak_fall = (unsigned)num; sawAny = true; }
		else if (key_equals(k, "bar_rise_ms"))   { s.bar_rise_ms = (unsigned)num; sawAny = true; }
		else if (key_equals(k, "bar_fall_ms"))   { s.bar_fall_ms = (unsigned)num; sawAny = true; }
		else if (key_equals(k, "gain"))          { s.gain = (unsigned)num; sawAny = true; }
		// 倾斜可以是负数，所以不能用上面那个已经钳到 0 的 num。
		else if (key_equals(k, "slope"))         { s.slope_db_per_oct = parse_int(val.get_ptr()); sawAny = true; }
		else if (key_equals(k, "gradient_mode")) { s.gradient_mode = (unsigned)num; sawAny = true; }
		else if (key_equals(k, "gradient_dir"))  { s.gradient_dir = (unsigned)num; sawAny = true; }
		else if (key_equals(k, "fft"))           { s.fft = (unsigned)num; sawAny = true; }
		else if (key_equals(k, "low_boost"))     { s.low_boost = (num != 0); sawAny = true; }
		else if (key_equals(k, "freq_min"))      { s.freq_min = (unsigned)num; sawAny = true; }
		else if (key_equals(k, "freq_max"))      { s.freq_max = (unsigned)num; sawAny = true; }
		else if (key_equals(k, "db_scale"))      { s.db_scale = (num != 0); sawAny = true; }
		else if (key_equals(k, "log_freq"))      { s.log_freq = (num != 0); sawAny = true; }
		else if (key_equals(k, "grid"))          { s.grid = (num != 0); sawAny = true; }
		else if (key_equals(k, "scale_freq"))    { s.scale_freq_pos = (unsigned)num; sawAny = true; }
		else if (key_equals(k, "scale_level"))   { s.scale_level_pos = (unsigned)num; sawAny = true; }
		else if (key_equals(k, "version"))       { sawAny = true; }
		else if (key_equals(k, "color.background")) { s.colors.background = parse_color(val.get_ptr(), s.colors.background); sawAny = true; }
		else if (key_equals(k, "color.bar_low"))    { s.colors.bar_low    = parse_color(val.get_ptr(), s.colors.bar_low);    sawAny = true; }
		else if (key_equals(k, "color.bar_mid"))    { s.colors.bar_mid    = parse_color(val.get_ptr(), s.colors.bar_mid);    sawAny = true; }
		else if (key_equals(k, "color.bar_high"))   { s.colors.bar_high   = parse_color(val.get_ptr(), s.colors.bar_high);   sawAny = true; }
		else if (key_equals(k, "color.peak"))       { s.colors.peak       = parse_color(val.get_ptr(), s.colors.peak);       sawAny = true; }
		else if (key_equals(k, "color.grid"))       { s.colors.grid       = parse_color(val.get_ptr(), s.colors.grid);       sawAny = true; }
		else if (key_equals(k, "color.scale"))      { s.colors.scale      = parse_color(val.get_ptr(), s.colors.scale);      sawAny = true; }
	}

	if (!sawAny) {
		error = "没有找到任何可识别的设置项";
		return false;
	}

	spectrum_settings_save(s);
	return true;
}

// ---------------------------------------------------------------------------
// 用户自定义外观预设
//
// 存在组件自己的 cfg_string 里 —— 它在配置文件中有自己的条目，但**不会**
// 出现在高级参数列表中，所以不用担心用户误改到内部格式。
// 每行一个预设，字段用 '|' 分隔：
//   name|bg|low|mid|high|peak|gridcolor|scalecolor          (0..7, 老格式到此为止)
//   |gradient_mode|gradient_dir|style|alpha|gap              (8..12)
//   |db_scale|log_freq|grid_on|scale_freq|scale_level|slope  (13..18)
//
// 老版本只写前 10 个字段，解析时"读到几个就套用几个"，所以旧预设仍然有效。
// ---------------------------------------------------------------------------
namespace {

	enum { kMaxUserPresets = 32, kPresetFields = 19 };

	struct t_user_preset {
		pfc::string8 name, bg, low, mid, high, peak, grid, scale;
		unsigned gmode = 0, gdir = 1;
		//! How many '|'-separated fields the line actually had. Everything past
		//! field 9 is optional and only applied when present.
		unsigned fields = 0;
		unsigned style = 1, alpha = 255, gap = 1;
		bool dbScale = true, logFreq = true, gridOn = false;
		unsigned scaleFreq = 0, scaleLevel = 0;
		int slope = 0;
	};

	bool preset_line_at(unsigned index, pfc::string8 & out) {
		const pfc::string8 all = g_userPresets.get();
		const char * p = all.get_ptr();
		unsigned i = 0;
		while (*p != 0) {
			const char * e = p;
			while (*e != 0 && *e != '\n') ++e;
			if (i == index) {
				out.set_string(p, (t_size)(e - p));
				return true;
			}
			++i;
			p = e;
			if (*p == '\n') ++p;
		}
		return false;
	}

	bool parse_preset_line(const char * line, t_user_preset & out) {
		pfc::string8 f[kPresetFields];
		unsigned n = 0;
		const char * p = line;
		while (n < (unsigned)kPresetFields) {
			const char * e = p;
			while (*e != 0 && *e != '|') ++e;
			f[n].set_string(p, (t_size)(e - p));
			++n;
			if (*e == 0) break;
			p = e + 1;
		}
		if (n < 10) return false; // 老格式的最小长度

		out.fields = n;
		out.name  = f[0].get_ptr();
		out.bg    = f[1].get_ptr();
		out.low   = f[2].get_ptr();
		out.mid   = f[3].get_ptr();
		out.high  = f[4].get_ptr();
		out.peak  = f[5].get_ptr();
		out.grid  = f[6].get_ptr();
		out.scale = f[7].get_ptr();
		out.gmode = (unsigned)parse_int(f[8].get_ptr());
		out.gdir  = (unsigned)parse_int(f[9].get_ptr());

		if (n > 10) out.style      = (unsigned)parse_int(f[10].get_ptr());
		if (n > 11) out.alpha      = (unsigned)parse_int(f[11].get_ptr());
		if (n > 12) out.gap        = (unsigned)parse_int(f[12].get_ptr());
		if (n > 13) out.dbScale    = parse_int(f[13].get_ptr()) != 0;
		if (n > 14) out.logFreq    = parse_int(f[14].get_ptr()) != 0;
		if (n > 15) out.gridOn     = parse_int(f[15].get_ptr()) != 0;
		if (n > 16) out.scaleFreq  = (unsigned)parse_int(f[16].get_ptr());
		if (n > 17) out.scaleLevel = (unsigned)parse_int(f[17].get_ptr());
		if (n > 18) out.slope      = parse_int(f[18].get_ptr());
		return !out.name.is_empty();
	}

	pfc::string8 make_preset_line(const char * name) {
		const t_spectrum_settings s = spectrum_settings_load();
		pfc::string8 line;
		line += name; line += "|";
		line += color_to_text(s.colors.background).get_ptr(); line += "|";
		line += color_to_text(s.colors.bar_low).get_ptr();    line += "|";
		line += color_to_text(s.colors.bar_mid).get_ptr();    line += "|";
		line += color_to_text(s.colors.bar_high).get_ptr();   line += "|";
		line += color_to_text(s.colors.peak).get_ptr();       line += "|";
		line += color_to_text(s.colors.grid).get_ptr();       line += "|";
		line += color_to_text(s.colors.scale).get_ptr();      line += "|";
		char buf[160];
		wsprintfA(buf, "%u|%u|%u|%u|%u|%d|%d|%d|%u|%u|%d",
			s.gradient_mode, s.gradient_dir,
			s.style, s.alpha, s.gap,
			s.db_scale ? 1 : 0, s.log_freq ? 1 : 0, s.grid ? 1 : 0,
			s.scale_freq_pos, s.scale_level_pos,
			s.slope_db_per_oct);
		line += buf;
		return line;
	}

	//! Drops the characters that would break the line format.
	pfc::string8 sanitize_preset_name(const char * name) {
		pfc::string8 out;
		if (name == nullptr) return out;
		for (const char * q = name; *q != 0; ++q) {
			if (*q == '|' || *q == '\n' || *q == '\r') continue;
			char tmp[2] = { *q, 0 };
			out += tmp;
		}
		return out;
	}

} // 匿名命名空间

unsigned spectrum_user_preset_count() {
	unsigned n = 0;
	pfc::string8 line;
	while (preset_line_at(n, line)) ++n;
	return n;
}

const char * spectrum_user_preset_name(unsigned index) {
	// Note: the returned pointer is only valid until the next call.
	static pfc::string8 name;
	name.set_string("", 0);
	pfc::string8 line;
	t_user_preset p;
	if (preset_line_at(index, line) && parse_preset_line(line.get_ptr(), p)) {
		name.set_string(p.name.get_ptr(), p.name.length());
	}
	return name.get_ptr();
}

bool spectrum_user_preset_apply(unsigned index) {
	pfc::string8 line;
	t_user_preset p;
	if (!preset_line_at(index, line)) return false;
	if (!parse_preset_line(line.get_ptr(), p)) return false;

	// 配色与渐变：所有格式都有。
	g_col_bg.set(p.bg.get_ptr());
	g_col_low.set(p.low.get_ptr());
	g_col_mid.set(p.mid.get_ptr());
	g_col_high.set(p.high.get_ptr());
	g_col_peak.set(p.peak.get_ptr());
	g_col_grid.set(p.grid.get_ptr());
	g_col_scale.set(p.scale.get_ptr());
	g_gradmode.set(p.gmode);
	g_graddir.set(p.gdir);

	// 完整外观：只有写过的字段才套用，所以老预设不会把样式重置成默认值。
	// 各 advconfig 工厂会按自己的 min/max 钳位。
	if (p.fields > 10) g_style.set(p.style);
	if (p.fields > 11) g_alpha.set(p.alpha);
	if (p.fields > 12) g_gap.set(p.gap);
	if (p.fields > 13) g_dbscale.set(p.dbScale);
	if (p.fields > 14) g_logfreq.set(p.logFreq);
	if (p.fields > 15) g_grid.set(p.gridOn);
	if (p.fields > 16) g_scale_freq.set(p.scaleFreq);
	if (p.fields > 17) g_scale_level.set(p.scaleLevel);
	if (p.fields > 18) g_slope.set(p.slope);
	return true;
}

bool spectrum_user_preset_save(const char * name) {
	const pfc::string8 clean = sanitize_preset_name(name);
	if (clean.is_empty()) return false;

	const pfc::string8 fresh = make_preset_line(clean.get_ptr());

	pfc::string8 out;
	bool replaced = false;
	unsigned n = 0;
	pfc::string8 line;
	while (preset_line_at(n, line)) {
		t_user_preset p;
		const bool sameName = parse_preset_line(line.get_ptr(), p)
			&& key_equals(p.name.get_ptr(), clean.get_ptr());
		if (sameName) {
			out += fresh.get_ptr();
			out += "\n";
			replaced = true;
		} else {
			out += line.get_ptr();
			out += "\n";
		}
		++n;
	}

	if (!replaced) {
		if (n >= (unsigned)kMaxUserPresets) return false;
		out += fresh.get_ptr();
		out += "\n";
	}

	g_userPresets.set(out.get_ptr());
	return true;
}

bool spectrum_user_preset_delete(unsigned index) {
	pfc::string8 out;
	bool found = false;
	unsigned n = 0;
	pfc::string8 line;
	while (preset_line_at(n, line)) {
		if (n == index) {
			found = true;
		} else {
			out += line.get_ptr();
			out += "\n";
		}
		++n;
	}
	if (!found) return false;

	g_userPresets.set(out.get_ptr());
	return true;
}
