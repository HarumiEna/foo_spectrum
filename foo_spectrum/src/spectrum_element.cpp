#include "stdafx.h"
#include "settings.h"
#include "settings_dialog.h"
#include "color_dialog.h"

#include <libPPUI/win32_op.h>     // WIN32_OP()
#include <helpers/atl-misc.h>     // ui_element_impl<>
#include <helpers/BumpableElem.h> // ui_element_impl_visualisation<>

#include <SDK/service_by_guid.h>

#include <commdlg.h> // ChooseColorW (linked against comdlg32.lib)

#include <algorithm>
#include <atomic>
#include <cmath>
#include <process.h>
#include <vector>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

// ---------------------------------------------------------------------------
// Spectrum Visualizer
//
// A UI element (panel) showing the spectrum of the currently playing audio.
// The FFT itself is performed by the foobar2000 core - we pull a normalized
// (0..1) spectrum through the visualisation_stream API and only do the band
// mapping, smoothing and drawing ourselves.
//
// Frame pacing uses a dedicated ticker thread that waits on a high-resolution
// waitable timer and posts a message to the UI thread. WM_TIMER was not usable
// here: it is only generated when the message queue is otherwise empty and its
// resolution is tied to the ~15.6 ms system clock tick, which caps a panel at
// roughly 60 fps no matter what interval you ask for.
// ---------------------------------------------------------------------------

namespace {

	// This is our UI element GUID - substitute with your own when reusing code.
	static constexpr GUID guid_spectrum_element =
		{ 0x9e3f7c41, 0x2a8b, 0x4d6e, { 0x9f, 0x10, 0x5c, 0x7a, 0x2e, 0x8b, 0x4d, 0x93 } };

	enum {
		kCmdSettingsPanel = 890,
		kCmdColorPanel = 895,
		kCmdOpenSettings = 900,
		kIdleIntervalMs = 1000,     // polling interval while the panel is hidden
		kMinFftSize = 4096,
		kMaxFftSize = 16384,
		kMinFps = 10,
		kMaxFps = 240,
		// Bands whose centre frequency is below this read from the long-FFT
		// spectrum; above it the short window keeps transients crisp.
		kLowCrossoverHz = 500,
		// How long a hidden panel keeps its visualisation stream before handing
		// it back. Staying alive for a few seconds is what makes switching back
		// to the panel instant instead of a stall.
		kHideGraceMs = 5000,
	};

	// Posted by the ticker thread; carries no payload.
	const UINT WM_APP_SPECTRUM_TICK = WM_APP + 0x53;

	// winmm is loaded lazily so the component does not gain a hard dependency on
	// it: only systems without high-resolution waitable timers need it.
	void system_timer_resolution(bool raise) {
		typedef UINT (WINAPI *t_period)(UINT);
		static HMODULE hWinmm = ::LoadLibraryW(L"winmm.dll");
		static t_period pBegin = hWinmm ? reinterpret_cast<t_period>(::GetProcAddress(hWinmm, "timeBeginPeriod")) : nullptr;
		static t_period pEnd   = hWinmm ? reinterpret_cast<t_period>(::GetProcAddress(hWinmm, "timeEndPeriod"))   : nullptr;
		if (raise) { if (pBegin) pBegin(1); }
		else       { if (pEnd)   pEnd(1);   }
	}

	// --- colour helpers ----------------------------------------------------

	inline uint32_t pixel_from_color(COLORREF c) {
		return 0xFF000000u
			| ((uint32_t)GetRValue(c) << 16)
			| ((uint32_t)GetGValue(c) << 8)
			| ((uint32_t)GetBValue(c));
	}

	// Alpha is applied by blending against the background, because the bar
	// colour is always drawn directly on top of the background.
	inline COLORREF blend_color(COLORREF fg, COLORREF bg, unsigned alpha) {
		if (alpha >= 255) return fg;
		if (alpha == 0) return bg;
		const unsigned inv = 255 - alpha;
		return RGB(
			(GetRValue(fg) * alpha + GetRValue(bg) * inv) / 255,
			(GetGValue(fg) * alpha + GetGValue(bg) * inv) / 255,
			(GetBValue(fg) * alpha + GetBValue(bg) * inv) / 255);
	}

	inline COLORREF lerp_color(COLORREF a, COLORREF b, float t) {
		if (t < 0.0f) t = 0.0f;
		if (t > 1.0f) t = 1.0f;
		return RGB(
			(int)(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t + 0.5f),
			(int)(GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t + 0.5f),
			(int)(GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t + 0.5f));
	}

	inline COLORREF hsv_to_rgb(float h, float s, float v) {
		h = fmodf(h, 360.0f);
		if (h < 0.0f) h += 360.0f;
		const float c = v * s;
		const float hp = h / 60.0f;
		const float x = c * (1.0f - fabsf(fmodf(hp, 2.0f) - 1.0f));
		float r = 0.0f, g = 0.0f, b = 0.0f;
		if (hp < 1.0f)      { r = c; g = x; }
		else if (hp < 2.0f) { r = x; g = c; }
		else if (hp < 3.0f) { g = c; b = x; }
		else if (hp < 4.0f) { g = x; b = c; }
		else if (hp < 5.0f) { r = x; b = c; }
		else                { r = c; b = x; }
		const float m = v - c;
		return RGB(
			(int)((r + m) * 255.0f + 0.5f),
			(int)((g + m) * 255.0f + 0.5f),
			(int)((b + m) * 255.0f + 0.5f));
	}

	// t = 0 at the low end, 1 at the high end.
	// Three stops (low -> mid -> high) so a green/yellow/red ramp stays clean
	// instead of turning muddy brown on the way from green to red.
	inline COLORREF gradient_color(const t_spectrum_settings & s, float t) {
		if (s.gradient_mode == 1) return hsv_to_rgb(t * 300.0f, 1.0f, 1.0f);
		if (t < 0.0f) t = 0.0f;
		if (t > 1.0f) t = 1.0f;
		if (t < 0.5f) return lerp_color(s.colors.bar_low, s.colors.bar_mid, t * 2.0f);
		return lerp_color(s.colors.bar_mid, s.colors.bar_high, (t - 0.5f) * 2.0f);
	}

	// Peak across channels of a single FFT bin.
	inline float bin_magnitude(const audio_sample * data, long bin, unsigned channels) {
		const audio_sample * frame = data + (size_t)bin * channels;
		float peak = 0.0f;
		for (unsigned c = 0; c < channels; ++c) {
			float v = (float)frame[c];
			if (v < 0.0f) v = -v;
			if (v > peak) peak = v;
		}
		return peak;
	}

	// Automatic FFT size: at least 8 input bins per displayed band, never below
	// 4096. A 2048-point FFT at 44.1 kHz has 21.5 Hz wide bins, which is wider
	// than many of the low bands on a logarithmic axis - every such band would
	// fall into the same bin and render as an identical bar.
	inline unsigned fft_size_for_bands(unsigned bands) {
		unsigned want = bands * 8u;
		unsigned size = kMinFftSize;
		while (size < want && size < kMaxFftSize) size <<= 1;
		return size;
	}

	// Ticks offered on the frequency axis; thinned at draw time when the panel
	// is too narrow to fit every label.
	const double kFreqTicks[] = {
		20.0, 30.0, 50.0, 70.0, 100.0, 200.0, 300.0, 500.0, 700.0,
		1000.0, 2000.0, 3000.0, 5000.0, 7000.0, 10000.0, 15000.0, 20000.0
	};

	const int kDbTicks[] = { 0, -12, -24, -36, -48, -60 };
	const int kLinearTicks[] = { 100, 75, 50, 25, 0 };

	// Maps a normalized level (0..1) to a bar height in pixels. Bars, peak
	// markers and the level scale all go through this so the tick marks line up
	// exactly with the bar tops they label.
	inline int level_to_pixels(float level, int plotH) {
		if (plotH <= 0) return 0;
		int h = (int)(level * (float)plotH + 0.5f);
		if (h < 0) h = 0;
		if (h > plotH) h = plotH;
		return h;
	}

	// Value of one band, read out of a spectrum whose `bins` linear bins span
	// 0..nyquist. Shared by the base (short FFT) and low-band (long FFT) spectra.
	float band_value_from_spectrum(const audio_sample * data, unsigned bins, unsigned channels,
	                               double lo, double hi, double nyquist) {
		if (data == nullptr || bins == 0 || channels == 0 || nyquist <= 0.0) return 0.0f;

		long first = (long)floor(lo / nyquist * (double)bins);
		long last  = (long)ceil (hi / nyquist * (double)bins);
		if (last <= first) last = first + 1;
		if (first < 0) first = 0;
		if (last > (long)bins) last = (long)bins;
		if (first >= (long)bins) first = (long)bins - 1;

		// Value of the continuous spectrum at the band centre. On a logarithmic
		// axis a low-frequency band is often narrower than a single FFT bin;
		// feeding every such band the bin it happens to fall into makes long
		// runs of bars come out with identical heights. Interpolating between
		// neighbouring bins instead gives each band its own sample of the curve.
		const double centre = 0.5 * (lo + hi);
		const double cpos = centre / nyquist * (double)bins;
		long ci = (long)floor(cpos);
		float ct = (float)(cpos - (double)ci);
		if (ci < 0) { ci = 0; ct = 0.0f; }
		if (ci >= (long)bins - 1) { ci = (long)bins - 1; ct = 0.0f; }
		const float c0 = bin_magnitude(data, ci, channels);
		const float c1 = (ci + 1 < (long)bins) ? bin_magnitude(data, ci + 1, channels) : c0;
		const float centreValue = c0 + (c1 - c0) * ct;

		// Band statistics, meaningful once the band spans more than one bin.
		// A blend of peak and mean keeps narrow tones visible without letting
		// broadband noise dominate the display.
		float peak = 0.0f, sum = 0.0f;
		unsigned count = 0;
		for (long i = first; i < last; ++i) {
			const float v = bin_magnitude(data, i, channels);
			if (v > peak) peak = v;
			sum += v;
			++count;
		}
		float rangeValue = 0.0f;
		if (count > 0) rangeValue = peak * 0.65f + (sum / (float)count) * 0.35f;

		// Narrow bands trust the interpolated sample, wide bands trust the
		// peak/mean over their bins; blend smoothly in between.
		const double widthInBins = (hi - lo) / nyquist * (double)bins;
		float alpha = (float)(widthInBins - 1.0);
		if (alpha < 0.0f) alpha = 0.0f;
		if (alpha > 1.0f) alpha = 1.0f;

		return centreValue + (rangeValue - centreValue) * alpha;
	}

	// -----------------------------------------------------------------------

	class CSpectrumWindow : public ui_element_instance, public CWindowImpl<CSpectrumWindow> {
	public:
		DECLARE_WND_CLASS_EX(TEXT("{9E3F7C41-2A8B-4D6E-9F10-5C7A2E8B4D93}"),
			CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS, (-1));

		void initialize_window(HWND parent) { WIN32_OP(Create(parent) != NULL); }

		BEGIN_MSG_MAP_EX(CSpectrumWindow)
			MSG_WM_CREATE(OnCreate)
			MSG_WM_DESTROY(OnDestroy)
			MSG_WM_PAINT(OnPaint)
			MSG_WM_ERASEBKGND(OnEraseBkgnd)
			MSG_WM_CONTEXTMENU(OnContextMenu)
			MSG_WM_LBUTTONDBLCLK(OnLButtonDblClk)
			MESSAGE_HANDLER(WM_APP_SPECTRUM_TICK, OnTick)
			MESSAGE_HANDLER(WM_SHOWWINDOW, OnShowWindow)
		END_MSG_MAP()

		CSpectrumWindow(ui_element_config::ptr cfg, ui_element_instance_callback_ptr callback)
			: m_config(cfg), m_callback(callback) {}

		~CSpectrumWindow() {
			stop_ticker();
			release_backbuffer();
			if (m_scaleFont.m_hFont != NULL) m_scaleFont.DeleteObject();
		}

		void set_configuration(ui_element_config::ptr config) { m_config = config; }
		ui_element_config::ptr get_configuration() { return m_config; }

		static GUID g_get_guid() { return guid_spectrum_element; }
		static GUID g_get_subclass() { return ui_element_subclass_playback_visualisation; }
		static void g_get_name(pfc::string_base & out) { out = "频谱可视化 (Spectrum Visualizer)"; }
		static ui_element_config::ptr g_get_default_configuration() {
			return ui_element_config::g_create_empty(g_get_guid());
		}
		static const char * g_get_description() {
			return "多波段实时音频频谱面板，配色可完全自定义。";
		}

		ui_element_min_max_info get_min_max_info() override {
			ui_element_min_max_info info;
			info.m_min_width = 120;
			info.m_min_height = 60;
			info.adjustForWindow(*this);
			return info;
		}

		void notify(const GUID & p_what, t_size p_param1, const void * p_param2, t_size p_param2size) override;

	private:
		ui_element_config::ptr m_config;

	protected:
		// Must stay protected - ImplementBumpableElem<> accesses m_callback.
		const ui_element_instance_callback_ptr m_callback;

	private:
		int  OnCreate(LPCREATESTRUCT lpcs);
		void OnDestroy();
		void OnPaint(CDCHandle dc);
		BOOL OnEraseBkgnd(CDCHandle dc);
		LRESULT OnTick(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL & bHandled);
		LRESULT OnShowWindow(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL & bHandled);
		void OnContextMenu(CWindow wnd, CPoint pt);
		void OnLButtonDblClk(UINT flags, CPoint pt);
		//! Brings the panel straight back up to speed when it becomes visible.
		void resume_now();

		// --- frame pacing ---------------------------------------------------
		static unsigned __stdcall tick_proc(void * param);
		void start_ticker();
		void stop_ticker();

		unsigned frame_interval_ms() const {
			unsigned fps = m_settings.fps;
			if (fps < kMinFps) fps = kMinFps;
			if (fps > kMaxFps) fps = kMaxFps;
			return 1000u / fps;
		}

		// --- rendering ------------------------------------------------------
		void ensure_stream();
		void release_stream();
		void update_spectrum();
		void ensure_scale_font(HDC dc);
		int  current_dpi() const;
		bool ensure_backbuffer(HDC refDC, int width, int height);
		void release_backbuffer();
		void render(HDC dc, const CRect & rc);
		void draw_scale(HDC dc, int width, int height,
		                int padL, int padT, int padR, int padB, int plotW, int plotH);
		double freq_to_fraction(double hz) const;

		// --- state ---------------------------------------------------------
		t_spectrum_settings m_settings;
		visualisation_stream::ptr m_stream;
		audio_chunk_fast_impl m_chunk;    // base spectrum (short FFT)
		audio_chunk_fast_impl m_chunkLow; // low-band spectrum (long FFT)
		bool m_haveLow = false;
		bool m_ever_got_real_data = false;

		std::vector<float> m_levels;  // smoothed level per band, 0..1
		std::vector<float> m_peaks;   // peak-hold value per band, 0..1
		unsigned m_band_count = 0;
		unsigned m_fft_size = kMinFftSize;
		unsigned m_fft_low = kMinFftSize;

		// Axis parameters of the last spectrum we processed, used to place the
		// frequency scale ticks.
		double m_axis_fmin = 20.0;
		double m_axis_fmax = 20000.0;

		// Double buffer. Everything - background, bars, grid AND the scale text -
		// is drawn into this DIB and presented with a single BitBlt. Painting the
		// scale text straight onto the window DC made the labels flicker: every
		// frame the blit wiped them and they were then redrawn in full view.
		HDC m_backDC = NULL;
		HBITMAP m_backBmp = NULL;
		HGDIOBJ m_backOldBmp = NULL;
		void * m_backBits = nullptr;
		int m_backW = 0, m_backH = 0;

		// Vertical gradient lookup, one entry per row of the plot area.
		std::vector<uint32_t> m_gradientLut;

		CFont m_scaleFont;
		int m_scaleFontDpi = 0;

		// --- ticker thread --------------------------------------------------
		HANDLE m_tickThread = nullptr;
		HANDLE m_tickStopEvent = nullptr;
		HANDLE m_tickWakeEvent = nullptr; // set to make the ticker fire immediately
		ULONGLONG m_hiddenSince = 0;      // GetTickCount64 when the panel got hidden
		std::atomic<unsigned> m_tickIntervalMs{33};
		std::atomic<bool> m_tickStop{false};
		std::atomic<bool> m_tickPending{false};
	};

	// --- ui_element_instance -----------------------------------------------

	void CSpectrumWindow::notify(const GUID & p_what, t_size p_param1, const void *, t_size) {
		if (p_what == ui_element_notify_visibility_changed) {
			if (p_param1 != 0) resume_now();
			InvalidateRect(NULL, FALSE);
			return;
		}
		if (p_what == ui_element_notify_colors_changed || p_what == ui_element_notify_font_changed) {
			// The scale font follows the screen DPI, not the host font, and is
			// rebuilt automatically when the DPI changes - a repaint is enough.
			InvalidateRect(NULL, FALSE);
		}
	}

	// Called whenever the panel becomes visible again. Without this the panel
	// would sit out the slow hidden-polling interval before producing its first
	// frame, which reads as a stall when switching tabs.
	void CSpectrumWindow::resume_now() {
		m_hiddenSince = 0;
		m_settings = spectrum_settings_load();
		m_tickIntervalMs.store(frame_interval_ms());
		if (m_hWnd != NULL) {
			update_spectrum();
			InvalidateRect(NULL, FALSE);
		}
		if (m_tickWakeEvent != nullptr) ::SetEvent(m_tickWakeEvent);
	}

	// --- ticker thread -----------------------------------------------------

	unsigned __stdcall CSpectrumWindow::tick_proc(void * param) {
		CSpectrumWindow * self = static_cast<CSpectrumWindow *>(param);

		// High-resolution waitable timers exist since Windows 10 1803. On older
		// systems we fall back to a regular waitable timer and ask winmm for a
		// 1 ms system clock so short intervals actually fire on time.
		HANDLE hTimer = ::CreateWaitableTimerExW(nullptr, nullptr,
			CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
		const bool highRes = (hTimer != nullptr);
		if (hTimer == nullptr) hTimer = ::CreateWaitableTimerW(nullptr, FALSE, nullptr);
		if (!highRes) system_timer_resolution(true);

		while (!self->m_tickStop.load()) {
			unsigned ms = self->m_tickIntervalMs.load();
			if (ms < 1) ms = 1;

			// Waits on three things: stop, the "run now" wake-up, and the pacing
			// timer. The wake event is auto-reset, so one SetEvent() yields exactly
			// one extra tick - used when the panel becomes visible again and must
			// not sit out the slow hidden-polling interval.
			if (hTimer != nullptr) {
				LARGE_INTEGER due;
				due.QuadPart = -static_cast<LONGLONG>(ms) * 10000LL; // relative, 100 ns units
				::SetWaitableTimer(hTimer, &due, 0, nullptr, nullptr, FALSE);
				HANDLE waits[3];
				waits[0] = self->m_tickStopEvent;
				waits[1] = self->m_tickWakeEvent;
				waits[2] = hTimer;
				const DWORD w = ::WaitForMultipleObjects(3, waits, FALSE, 5000);
				if (w == WAIT_OBJECT_0) break;
			} else {
				HANDLE waits[2];
				waits[0] = self->m_tickStopEvent;
				waits[1] = self->m_tickWakeEvent;
				const DWORD w = ::WaitForMultipleObjects(2, waits, FALSE, ms);
				if (w == WAIT_OBJECT_0) break;
			}

			if (self->m_tickStop.load()) break;

			// Never let ticks pile up in the message queue: if the UI thread has
			// not consumed the previous one yet, skip this beat.
			if (!self->m_tickPending.exchange(true)) {
				if (self->m_hWnd != nullptr) ::PostMessageW(self->m_hWnd, WM_APP_SPECTRUM_TICK, 0, 0);
				else self->m_tickPending.store(false);
			}
		}

		if (!highRes) system_timer_resolution(false);
		if (hTimer != nullptr) ::CloseHandle(hTimer);
		return 0;
	}

	void CSpectrumWindow::start_ticker() {
		if (m_tickThread != nullptr) return;
		m_tickStopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
		// auto-reset: one SetEvent() -> exactly one immediate tick
		m_tickWakeEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (m_tickStopEvent == nullptr || m_tickWakeEvent == nullptr) {
			if (m_tickStopEvent != nullptr) { ::CloseHandle(m_tickStopEvent); m_tickStopEvent = nullptr; }
			if (m_tickWakeEvent != nullptr) { ::CloseHandle(m_tickWakeEvent); m_tickWakeEvent = nullptr; }
			return;
		}
		m_tickStop.store(false);
		m_tickPending.store(false);
		m_tickThread = reinterpret_cast<HANDLE>(
			_beginthreadex(nullptr, 0, &CSpectrumWindow::tick_proc, this, 0, nullptr));
		if (m_tickThread == nullptr) {
			::CloseHandle(m_tickStopEvent);
			m_tickStopEvent = nullptr;
			::CloseHandle(m_tickWakeEvent);
			m_tickWakeEvent = nullptr;
		}
	}

	void CSpectrumWindow::stop_ticker() {
		if (m_tickThread == nullptr) return;
		m_tickStop.store(true);
		if (m_tickStopEvent != nullptr) ::SetEvent(m_tickStopEvent);
		if (m_tickWakeEvent != nullptr) ::SetEvent(m_tickWakeEvent);
		::WaitForSingleObject(m_tickThread, 5000);
		::CloseHandle(m_tickThread);
		m_tickThread = nullptr;
		if (m_tickStopEvent != nullptr) {
			::CloseHandle(m_tickStopEvent);
			m_tickStopEvent = nullptr;
		}
		if (m_tickWakeEvent != nullptr) {
			::CloseHandle(m_tickWakeEvent);
			m_tickWakeEvent = nullptr;
		}
	}

	// --- window messages ---------------------------------------------------

	int CSpectrumWindow::OnCreate(LPCREATESTRUCT) {
		m_settings = spectrum_settings_load();
		m_tickIntervalMs.store(frame_interval_ms());
		start_ticker();
		return 0;
	}

	void CSpectrumWindow::OnDestroy() {
		stop_ticker();
		release_backbuffer();
		if (m_scaleFont.m_hFont != NULL) {
			m_scaleFont.DeleteObject();
			m_scaleFontDpi = 0;
		}
		release_stream();
		SetMsgHandled(FALSE);
	}

	void CSpectrumWindow::OnPaint(CDCHandle) {
		CPaintDC dc(*this);
		CRect rc;
		if (!GetClientRect(&rc)) return;
		render(dc, rc);
	}

	BOOL CSpectrumWindow::OnEraseBkgnd(CDCHandle) {
		// Everything is painted in OnPaint; erasing here would only flicker.
		return TRUE;
	}

	LRESULT CSpectrumWindow::OnTick(UINT, WPARAM, LPARAM, BOOL & bHandled) {
		bHandled = TRUE;
		m_tickPending.store(false);

		if (m_hWnd == NULL) return 0;

		if (!IsWindowVisible()) {
			const ULONGLONG now = ::GetTickCount64();
			if (m_hiddenSince == 0) {
				// Drop to the slow poll immediately so a hidden panel costs
				// nothing, but keep the visualisation stream for a grace period
				// so switching back to the panel is instant.
				m_hiddenSince = now;
				m_tickIntervalMs.store(kIdleIntervalMs);
			} else if (m_stream.is_valid() && (now - m_hiddenSince) > kHideGraceMs) {
				// Parked long enough: hand the stream back so the core stops
				// producing FFT data for a panel nobody is looking at.
				release_stream();
			}
			return 0;
		}

		m_hiddenSince = 0;
		// Re-arm the pacing at the configured rate (cheap; only writes an atomic).
		m_tickIntervalMs.store(frame_interval_ms());

		update_spectrum();
		InvalidateRect(NULL, FALSE);
		return 0;
	}

	LRESULT CSpectrumWindow::OnShowWindow(UINT, WPARAM wParam, LPARAM, BOOL & bHandled) {
		bHandled = FALSE; // let the default handling run as well
		if (wParam != 0) resume_now();
		return 0;
	}

	void CSpectrumWindow::OnLButtonDblClk(UINT, CPoint) {
		// The visualisation subclass advertises fullscreen support, so hook the
		// usual double-click gesture up to it.
		auto api = ui_element_common_methods_v2::tryGet();
		if (api.is_empty()) return;
		auto element = service_by_guid<ui_element>(guid_spectrum_element);
		if (element.is_empty()) return;
		api->toggle_fullscreen(element, ::GetParent(m_hWnd));
	}

	void CSpectrumWindow::OnContextMenu(CWindow, CPoint pt) {
		if (pt.x == -1 && pt.y == -1) {
			CRect rc;
			if (GetWindowRect(&rc)) pt = rc.CenterPoint();
		}

		CMenu menu;
		if (!menu.CreatePopupMenu()) { SetMsgHandled(FALSE); return; }

		// The item id must be cast: CMenu::AppendMenu has both an HMENU and a
		// UINT_PTR overload, so a bare 0 is ambiguous.
		//
		// Everything colour related lives in its own window, so it is reachable
		// from one entry instead of a pile of submenus here.
		menu.AppendMenu(MF_STRING, (UINT_PTR)kCmdSettingsPanel, _T("设置面板…"));
		menu.AppendMenu(MF_STRING, (UINT_PTR)kCmdColorPanel, _T("颜色设置…"));
		menu.AppendMenu(MF_STRING, (UINT_PTR)kCmdOpenSettings, _T("高级设置…"));

		::SetForegroundWindow(m_hWnd);
		const UINT cmd = (UINT)menu.TrackPopupMenu(
			TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, m_hWnd);

		if (cmd == kCmdSettingsPanel) {
			spectrum_open_settings_window(m_hWnd);
		} else if (cmd == kCmdColorPanel) {
			spectrum_open_color_window(m_hWnd);
		} else if (cmd == kCmdOpenSettings) {
			spectrum_settings_show();
		}
		SetMsgHandled(TRUE);
	}

	// --- visualisation plumbing --------------------------------------------

	void CSpectrumWindow::ensure_stream() {
		if (m_stream.is_valid()) return;
		try {
			auto manager = visualisation_manager::get();
			if (manager.is_empty()) return;
			manager->create_stream(m_stream, visualisation_manager::KStreamFlagNewFFT);
			m_ever_got_real_data = false;

			// Prefer the front channels only - a 7.1 downmix makes the display
			// jump around for material that has little going on in the rears.
			visualisation_stream_v2::ptr v2;
			if (m_stream->service_query_t(v2)) {
				v2->set_channel_mode(visualisation_stream_v2::channel_mode_frontonly);
			}
		} catch (...) {
			m_stream.release();
		}
	}

	void CSpectrumWindow::release_stream() {
		m_stream.release();
		m_ever_got_real_data = false;
	}

	void CSpectrumWindow::update_spectrum() {
		m_settings = spectrum_settings_load();

		const unsigned bands = m_settings.bands;
		if (m_band_count != bands) {
			m_band_count = bands;
			m_levels.assign(bands, 0.0f);
			m_peaks.assign(bands, 0.0f);
		}
		if (m_band_count == 0) return;

		m_fft_size = (m_settings.fft != 0) ? m_settings.fft : fft_size_for_bands(m_band_count);

		// Second, longer window used for the low bands. A single long FFT would
		// smear transients everywhere; a single short one cannot resolve the
		// bottom octaves at all - at 4096 points the 64 bands between 20 and
		// 112 Hz span only ~15 bins, so all those bars can do is draw a smooth
		// ramp. Running both and picking per band gives resolution where it is
		// needed while highs keep their transients.
		unsigned fftLow = kMaxFftSize;
		if (fftLow < m_fft_size * 2) fftLow = m_fft_size * 2;
		if (fftLow > kMaxFftSize) fftLow = kMaxFftSize;
		if (!m_settings.low_boost) fftLow = m_fft_size;
		m_fft_low = fftLow;

		auto playback = playback_control::get();
		const bool playing = playback.is_valid() && playback->is_playing();

		bool got = false;
		m_haveLow = false;
		if (playing) {
			ensure_stream();
			if (m_stream.is_valid()) {
				double now = 0.0;
				if (m_stream->get_absolute_time(now)) {
					try {
						if (m_stream->get_spectrum_absolute(m_chunk, now, m_fft_size)) {
							got = true;
							m_ever_got_real_data = true;
						} else if (!m_ever_got_real_data) {
							// Very start of a track: the core has not buffered FFT
							// input yet. Show the placeholder spectrum rather than
							// an empty panel for a few frames.
							m_stream->make_fake_spectrum_absolute(m_chunk, now, m_fft_size);
							got = true;
						}
					} catch (...) {
						got = false;
					}
					if (got && m_fft_low > m_fft_size) {
						try {
							if (m_stream->get_spectrum_absolute(m_chunkLow, now, m_fft_low)) {
								m_haveLow = true;
							}
						} catch (...) {
							m_haveLow = false; // fall back to the base spectrum
						}
					}
				}
			}
		} else {
			release_stream();
		}

		// Peak-hold fall speed, in normalized units per displayed frame.
		const unsigned fps = (m_settings.fps > 0) ? m_settings.fps : 30;
		const float fallPerFrame = (m_settings.peak_fall / 100.0f) / (float)fps;

		// Bar attack/release, expressed as time constants in milliseconds so the
		// visual speed does not change with the refresh rate. Both are plain
		// exponential smoothing, and 0 ms means "no smoothing at all".
		const float dt = 1.0f / (float)fps;
		const float riseTau = (float)m_settings.bar_rise_ms / 1000.0f;
		const float fallTau = (float)m_settings.bar_fall_ms / 1000.0f;
		const float riseAlpha = (riseTau < 0.0005f) ? 1.0f : (1.0f - expf(-dt / riseTau));
		const float fallAlpha = (fallTau < 0.0005f) ? 1.0f : (1.0f - expf(-dt / fallTau));

		const bool haveData = got
			&& m_chunk.get_sample_count() > 0
			&& m_chunk.get_channels() > 0
			&& m_chunk.get_data() != nullptr;

		if (haveData) {
			const audio_sample * data = m_chunk.get_data();
			const unsigned bins = (unsigned)m_chunk.get_sample_count();
			const unsigned channels = (unsigned)m_chunk.get_channels();

			const bool lowValid = m_haveLow
				&& m_chunkLow.get_sample_count() > 0
				&& m_chunkLow.get_channels() > 0
				&& m_chunkLow.get_data() != nullptr;
			const audio_sample * lowData = lowValid ? m_chunkLow.get_data() : nullptr;
			const unsigned lowBins = lowValid ? (unsigned)m_chunkLow.get_sample_count() : 0;
			const unsigned lowChannels = lowValid ? (unsigned)m_chunkLow.get_channels() : 0;

			double srate = (double)m_chunk.get_srate();
			if (srate <= 0.0) srate = 44100.0;
			const double nyquist = srate * 0.5;
			const float gain = (float)m_settings.gain / 100.0f;

			// Displayed frequency range. Bands outside it are simply not drawn,
			// and the selected span is stretched across the whole panel width -
			// so setting e.g. 20-200 Hz turns all 256 bars into a bass zoom.
			double fmin = (double)m_settings.freq_min;
			double fmax = (double)m_settings.freq_max;
			const double fullMax = (nyquist < 20000.0) ? nyquist : 20000.0;
			if (fmax > nyquist) fmax = nyquist;
			if (fmin < 20.0) fmin = 20.0;
			if (fmax <= fmin * 1.05) { fmin = 20.0; fmax = fullMax; }
			const double logRatio = fmax / fmin;

			// Crossover between the short (transient-friendly) and long
			// (resolution-friendly) spectrum. Zoomed into a narrow range the long
			// window is used everywhere: there, resolution is what matters and the
			// short window would badly under-sample the stretched view.
			const double zoomRatio = (fmin > 0.0) ? (fmax / fmin) : 1.0;
			const double lowCrossover = (zoomRatio < 20.0) ? fmax : (double)kLowCrossoverHz;

			// Remember the axis for the scale ticks.
			m_axis_fmin = fmin;
			m_axis_fmax = fmax;

			for (unsigned b = 0; b < m_band_count; ++b) {
				double lo = 0.0, hi = 0.0;
				if (m_settings.log_freq) {
					lo = fmin * pow(logRatio, (double)b / (double)m_band_count);
					hi = fmin * pow(logRatio, (double)(b + 1) / (double)m_band_count);
				} else {
					lo = fmin + (fmax - fmin) * (double)b / (double)m_band_count;
					hi = fmin + (fmax - fmin) * (double)(b + 1) / (double)m_band_count;
				}

				// Low bands read the long-FFT spectrum; everything above the
				// crossover uses the short one so highs keep their transients.
				const double centre = 0.5 * (lo + hi);
				const bool useLow = lowValid && centre < lowCrossover;
				float level = band_value_from_spectrum(
					useLow ? lowData : data,
					useLow ? lowBins : bins,
					useLow ? lowChannels : channels,
					lo, hi, nyquist);
				level *= gain;

				if (m_settings.db_scale) {
					const float db = 20.0f * log10f(level > 1.0e-5f ? level : 1.0e-5f);
					level = (db + 60.0f) / 60.0f;
					if (level < 0.0f) level = 0.0f;
				}
				if (level > 1.0f) level = 1.0f;

				float & current = m_levels[b];
				if (level >= current) current += (level - current) * riseAlpha;
				else                  current += (level - current) * fallAlpha;

				float & hold = m_peaks[b];
				if (current >= hold) hold = current;
				else {
					hold -= fallPerFrame;
					if (hold < 0.0f) hold = 0.0f;
				}
			}
		} else {
			// Idle: fade everything out smoothly.
			for (unsigned b = 0; b < m_band_count; ++b) {
				float & current = m_levels[b];
				current += (0.0f - current) * fallAlpha;
				if (current < 0.001f) current = 0.0f;

				float & hold = m_peaks[b];
				hold -= fallPerFrame;
				if (hold < 0.0f) hold = 0.0f;
			}
		}
	}

	// --- scale helpers -----------------------------------------------------

	int CSpectrumWindow::current_dpi() const {
		typedef UINT (WINAPI *t_GetDpiForWindow)(HWND);
		static t_GetDpiForWindow pGetDpi = reinterpret_cast<t_GetDpiForWindow>(
			::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
		UINT dpi = (pGetDpi != nullptr && m_hWnd != nullptr) ? pGetDpi(m_hWnd) : 0;
		if (dpi == 0) dpi = 96;
		return (int)dpi;
	}

	void CSpectrumWindow::ensure_scale_font(HDC dc) {
		const int dpi = current_dpi();
		if (m_scaleFont.m_hFont != NULL && m_scaleFontDpi == dpi) return;
		if (m_scaleFont.m_hFont != NULL) m_scaleFont.DeleteObject();

		const int height = -MulDiv(8, dpi, 72); // 8 pt, DPI aware
		if (m_scaleFont.CreateFont(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
			DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
			CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, _T("Segoe UI")) == NULL) {
			// Fall back to whatever the DC provides if font creation failed.
			m_scaleFontDpi = 0;
			return;
		}
		m_scaleFontDpi = dpi;
		(void)dc;
	}

	// Maps a frequency to 0..1 across the visible axis.
	double CSpectrumWindow::freq_to_fraction(double hz) const {
		if (m_settings.log_freq) {
			if (hz <= m_axis_fmin) return 0.0;
			if (hz >= m_axis_fmax) return 1.0;
			return log(hz / m_axis_fmin) / log(m_axis_fmax / m_axis_fmin);
		}
		const double span = m_axis_fmax - m_axis_fmin;
		if (span <= 0.0) return hz >= m_axis_fmax ? 1.0 : 0.0;
		const double f = (hz - m_axis_fmin) / span;
		if (f <= 0.0) return 0.0;
		if (f >= 1.0) return 1.0;
		return f;
	}

	// --- back buffer -------------------------------------------------------

	bool CSpectrumWindow::ensure_backbuffer(HDC refDC, int width, int height) {
		if (m_backDC != NULL && m_backW == width && m_backH == height && m_backBits != nullptr) return true;
		release_backbuffer();

		HDC dc = ::CreateCompatibleDC(refDC);
		if (dc == NULL) return false;

		BITMAPINFO bmi = {};
		bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bmi.bmiHeader.biWidth = width;
		bmi.bmiHeader.biHeight = -height; // top-down, matching our row order
		bmi.bmiHeader.biPlanes = 1;
		bmi.bmiHeader.biBitCount = 32;
		bmi.bmiHeader.biCompression = BI_RGB;

		void * bits = nullptr;
		HBITMAP bmp = ::CreateDIBSection(refDC, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
		if (bmp == NULL || bits == nullptr) {
			if (bmp != NULL) ::DeleteObject(bmp);
			::DeleteDC(dc);
			return false;
		}

		m_backOldBmp = ::SelectObject(dc, bmp);
		m_backDC = dc;
		m_backBmp = bmp;
		m_backBits = bits;
		m_backW = width;
		m_backH = height;
		return true;
	}

	void CSpectrumWindow::release_backbuffer() {
		if (m_backDC != NULL && m_backOldBmp != NULL) {
			::SelectObject(m_backDC, m_backOldBmp);
			m_backOldBmp = NULL;
		}
		if (m_backBmp != NULL) { ::DeleteObject(m_backBmp); m_backBmp = NULL; }
		if (m_backDC != NULL) { ::DeleteDC(m_backDC); m_backDC = NULL; }
		m_backBits = nullptr;
		m_backW = m_backH = 0;
	}

	// --- rendering ---------------------------------------------------------

	void CSpectrumWindow::render(HDC dc, const CRect & rc) {
		const int width = rc.Width();
		const int height = rc.Height();
		if (width <= 0 || height <= 0) return;
		if (!ensure_backbuffer(dc, width, height)) return;

		const HDC mem = m_backDC;
		uint32_t * pixels = static_cast<uint32_t *>(m_backBits);

		const t_spectrum_settings & s = m_settings;
		const uint32_t background = pixel_from_color(s.colors.background);
		std::fill(pixels, pixels + (size_t)width * (size_t)height, background);

		// --- reserve margins for the scales --------------------------------
		// Each scale can sit on either of its two sides, so the plot is inset by
		// up to four independent margins.
		const unsigned freqPos = s.scale_freq_pos;   // 0 off, 1 below, 2 above
		const unsigned levelPos = s.scale_level_pos; // 0 off, 1 left, 2 right
		int padL = 0, padT = 0, padR = 0, padB = 0;
		if (freqPos != scale_off || levelPos != scale_off) {
			ensure_scale_font(mem);
			HGDIOBJ oldFont = (m_scaleFont.m_hFont != NULL)
				? ::SelectObject(mem, m_scaleFont.m_hFont) : NULL;
			SIZE sz = { 0, 0 };
			TEXTMETRICW tm = {};
			::GetTextExtentPoint32A(mem, "-60", 3, &sz);
			::GetTextMetricsW(mem, &tm);
			if (oldFont != NULL) ::SelectObject(mem, oldFont);

			if (freqPos == scale_primary)        padB = tm.tmHeight + 6;
			else if (freqPos == scale_secondary) padT = tm.tmHeight + 6;
			if (levelPos == scale_primary)        padL = sz.cx + 10;
			else if (levelPos == scale_secondary) padR = sz.cx + 10;

			// Degenerate panels: drop the margin rather than eat the spectrum.
			if (padL > width / 3) padL = 0;
			if (padR > width / 3) padR = 0;
			if (padT > height / 3) padT = 0;
			if (padB > height / 3) padB = 0;
		}

		const int plotX0 = padL, plotY0 = padT;
		const int plotW = width - padL - padR;
		const int plotH = height - padT - padB;
		const int plotRight = plotX0 + plotW;
		const int plotBottom = plotY0 + plotH;

		// --- vertical gradient lookup --------------------------------------
		// Rebuilt every frame; row 0 is the top of the plot, so t runs from 1 at
		// the top down to 0 at the bottom. That gives the classic spectrum look:
		// low colour at the bottom, high colour at the top, tall bars sweeping
		// through the whole ramp.
		const bool verticalGradient = (s.gradient_dir == gradient_vertical);
		if (verticalGradient && plotH > 0) {
			m_gradientLut.resize((size_t)plotH);
			const COLORREF bg = s.colors.background;
			const float denom = (plotH > 1) ? (float)(plotH - 1) : 1.0f;
			for (int y = 0; y < plotH; ++y) {
				const float t = (float)(plotH - 1 - y) / denom;
				m_gradientLut[y] = pixel_from_color(blend_color(gradient_color(s, t), bg, s.alpha));
			}
		}

		// Optional reference grid: horizontal lines only. Vertical lines fought
		// with the bars - themselves vertical - and just made the plot busier.
		if (s.grid && plotW > 0 && plotH > 0) {
			const uint32_t gridPixel = pixel_from_color(s.colors.grid);
			for (int i = 1; i < 8; ++i) {
				uint32_t * row = pixels + (size_t)(plotY0 + (plotH * i) / 8) * width + plotX0;
				for (int x = 0; x < plotW; ++x) row[x] = gridPixel;
			}
		}

		const unsigned bands = m_band_count;
		if (bands > 0 && plotW > 0 && plotH > 0) {
			const int gap = (int)s.gap;
			const double bandWidth = (double)plotW / (double)bands;
			const COLORREF backgroundCol = s.colors.background;
			const bool outline = (s.style == 2);
			const bool peakHold = (s.style == 1);
			const uint32_t peakPixel =
				pixel_from_color(blend_color(s.colors.peak, backgroundCol, s.alpha));

			for (unsigned b = 0; b < bands; ++b) {
				const int x0 = plotX0 + (int)(b * bandWidth);
				const int x1 = plotX0 + (int)((b + 1) * bandWidth);
				if (x1 <= x0) continue; // sub-pixel band on a very narrow panel

				int barWidth = x1 - x0 - gap;
				if (barWidth < 1) barWidth = 1;
				if (x0 + barWidth > plotRight) barWidth = plotRight - x0;
				if (barWidth <= 0) continue;

				float level = m_levels[b];
				if (!(level > 0.0f)) level = 0.0f;
				if (level > 1.0f) level = 1.0f;

				// Colour for the non-vertical modes: one colour for the whole bar.
				uint32_t flatPixel = 0;
				if (!verticalGradient) {
					const float t = (s.gradient_dir == gradient_by_level)
						? level
						: (bands > 1 ? (float)b / (float)(bands - 1) : 0.0f);
					flatPixel = pixel_from_color(
						blend_color(gradient_color(s, t), backgroundCol, s.alpha));
				}

				// Rows inside the plot are kept plot-relative (0 = top of the plot)
				// so the gradient lookup stays valid wherever the plot starts, and
				// plotY0 is added only when indexing the bitmap.
				const int barHeight = level_to_pixels(level, plotH);
				const int relTop = plotH - barHeight;

				if (outline) {
					if (barHeight > 0) {
						const uint32_t topPixel = verticalGradient ? m_gradientLut[relTop] : flatPixel;
						uint32_t * topRow = pixels + (size_t)(plotY0 + relTop) * width + x0;
						for (int i = 0; i < barWidth; ++i) topRow[i] = topPixel;
						for (int rel = relTop; rel < plotH; ++rel) {
							const uint32_t px = verticalGradient ? m_gradientLut[rel] : flatPixel;
							const size_t base = (size_t)(plotY0 + rel) * width + x0;
							pixels[base] = px;
							pixels[base + barWidth - 1] = px;
						}
					}
				} else {
					for (int rel = relTop; rel < plotH; ++rel) {
						if (rel < 0) continue;
						const uint32_t px = verticalGradient ? m_gradientLut[rel] : flatPixel;
						uint32_t * row = pixels + (size_t)(plotY0 + rel) * width + x0;
						for (int i = 0; i < barWidth; ++i) row[i] = px;
					}
				}

				if (peakHold && m_peaks[b] > 0.002f) {
					float holdValue = m_peaks[b];
					if (holdValue > 1.0f) holdValue = 1.0f;
					const int peakRel = plotH - level_to_pixels(holdValue, plotH);
					for (int dy = 0; dy < 2; ++dy) {
						const int rel = peakRel + dy;
						if (rel < 0 || rel >= plotH) continue;
						uint32_t * row = pixels + (size_t)(plotY0 + rel) * width + x0;
						for (int i = 0; i < barWidth; ++i) row[i] = peakPixel;
					}
				}
			}
		}

		// The scales go into the same off-screen bitmap, so labels are presented
		// atomically together with the bars and cannot flicker.
		if ((freqPos != scale_off || levelPos != scale_off) && plotW > 0 && plotH > 0) {
			draw_scale(mem, width, height, padL, padT, padR, padB, plotW, plotH);
		}

		// Single presentation of the complete frame.
		::BitBlt(dc, 0, 0, width, height, mem, 0, 0, SRCCOPY);
	}

	void CSpectrumWindow::draw_scale(HDC dc, int width, int height,
	                                 int padL, int padT, int padR, int padB, int plotW, int plotH) {
		const t_spectrum_settings & s = m_settings;
		const COLORREF col = s.colors.scale;
		const int plotX0 = padL, plotY0 = padT;
		const int plotRight = plotX0 + plotW;
		const int plotBottom = plotY0 + plotH;

		HGDIOBJ oldFont = NULL;
		if (m_scaleFont.m_hFont != NULL) oldFont = ::SelectObject(dc, m_scaleFont.m_hFont);
		const int oldBk = ::SetBkMode(dc, TRANSPARENT);
		const COLORREF oldText = ::SetTextColor(dc, col);
		HPEN pen = ::CreatePen(PS_SOLID, 1, col);
		HGDIOBJ oldPen = ::SelectObject(dc, pen);

		// --- frequency axis: below the plot, or above it --------------------
		const int freqPad = (s.scale_freq_pos == scale_primary) ? padB
		                  : (s.scale_freq_pos == scale_secondary) ? padT : 0;
		if (freqPad > 0) {
			const bool above = (s.scale_freq_pos == scale_secondary);
			// Ticks always grow outwards, away from the plot.
			const int tickFrom = above ? (plotY0 - 1) : plotBottom;
			const int tickTo   = above ? (plotY0 - 4) : (plotBottom + 3);
			const int labelY   = above ? (plotY0 - 4) : (plotBottom + 4);

			int lastRight = -10000;
			for (double hz : kFreqTicks) {
				const double frac = freq_to_fraction(hz);
				// Skip ticks outside an axis that is not log-mapped from 20 Hz.
				if (!s.log_freq && hz > m_axis_fmax) continue;
				if (frac <= 0.0 || frac > 1.0) continue;

				const int x = plotX0 + (int)(frac * (double)plotW + 0.5);

				pfc::string8 label;
				if (hz < 1000.0) label << (int)hz;
				else             label << (int)(hz / 1000.0) << "k";

				SIZE sz = { 0, 0 };
				::GetTextExtentPoint32A(dc, label.get_ptr(), (int)label.length(), &sz);
				int tx = x - sz.cx / 2;
				if (tx < plotX0) tx = plotX0;
				if (tx + sz.cx > plotRight) tx = plotRight - sz.cx;
				if (tx <= lastRight + 4) continue; // too crowded, skip this label

				::MoveToEx(dc, x, tickFrom, NULL);
				::LineTo(dc, x, tickTo);
				::TextOutA(dc, tx, above ? (labelY - sz.cy) : labelY,
					label.get_ptr(), (int)label.length());
				lastRight = tx + sz.cx;
			}
			// Unit hint at the far right.
			SIZE sz = { 0, 0 };
			::GetTextExtentPoint32A(dc, "Hz", 2, &sz);
			if (plotRight - sz.cx > lastRight + 4) {
				::TextOutA(dc, plotRight - sz.cx - 1, above ? (labelY - sz.cy) : labelY, "Hz", 2);
			}
		}

		// --- level axis: left of the plot, or right of it -------------------
		const int levelPad = (s.scale_level_pos == scale_primary) ? padL
		                   : (s.scale_level_pos == scale_secondary) ? padR : 0;
		if (levelPad > 0) {
			const bool right = (s.scale_level_pos == scale_secondary);
			const int tickFrom = right ? (plotRight - 2) : (plotX0 - 3);
			const int tickTo   = right ? (plotRight + 3) : (plotX0 + 2);

			const int tickCount = s.db_scale ? (int)(sizeof(kDbTicks) / sizeof(kDbTicks[0]))
			                                 : (int)(sizeof(kLinearTicks) / sizeof(kLinearTicks[0]));
			for (int i = 0; i < tickCount; ++i) {
				double level;
				pfc::string8 label;
				if (s.db_scale) {
					const int db = kDbTicks[i];
					level = (db + 60.0) / 60.0; // must match the dB mapping above
					label << db;
				} else {
					const int pct = kLinearTicks[i];
					level = pct / 100.0;
					label << pct << "%";
				}
				if (level < 0.0 || level > 1.0) continue;

				// Same mapping the bars use, so tick marks line up exactly with
				// the bar tops they label.
				int y = plotBottom - level_to_pixels((float)level, plotH);
				if (y >= plotBottom) y = plotBottom - 1;
				if (y < plotY0) y = plotY0;
				::MoveToEx(dc, tickFrom, y, NULL);
				::LineTo(dc, tickTo, y);

				SIZE sz = { 0, 0 };
				::GetTextExtentPoint32A(dc, label.get_ptr(), (int)label.length(), &sz);
				int tx = right ? (plotRight + 5) : (plotX0 - 5 - sz.cx);
				if (tx < 0) tx = 0;
				if (tx + sz.cx > width) tx = width - sz.cx;
				int ty = y - sz.cy / 2;
				if (ty < 0) ty = 0;
				if (ty + sz.cy > height) ty = height - sz.cy;
				::TextOutA(dc, tx, ty, label.get_ptr(), (int)label.length());
			}
		}

		::SelectObject(dc, oldPen);
		::DeleteObject(pen);
		::SetTextColor(dc, oldText);
		::SetBkMode(dc, oldBk);
		if (oldFont != NULL) ::SelectObject(dc, oldFont);
	}

	// Registers the element. ui_element_impl_visualisation<> additionally sets
	// the fullscreen flag and generates the View -> Visualizations menu entry.
	class ui_element_spectrum : public ui_element_impl_visualisation<CSpectrumWindow> {};

	static service_factory_single_t<ui_element_spectrum> g_spectrum_element_factory;

} // anonymous namespace
