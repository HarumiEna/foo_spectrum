#include "stdafx.h"
#include "settings.h"
#include "settings_dialog.h"
#include "ui_theme.h"

#include <commdlg.h>

#include <deque>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Settings window.
//
// Built programmatically rather than from a dialog resource: the component has
// no .rc file, and doing it in code keeps every string in UTF-8 C++ source where
// the /utf-8 switch already guarantees correct encoding. Layout is expressed in
// 96-dpi pixels and scaled by the actual DPI at creation time.
// ---------------------------------------------------------------------------

namespace {

	// --- layout, in 96-dpi pixels ------------------------------------------
	enum {
		kPad = 12,
		kGrpW = 428,               // section frame width
		kClientW = kPad * 2 + kGrpW,
		kRowH = 28,
		kCtlH = 22,
		kLabelW = 68,
		kEditW = 64,
		kComboW = 118,
		kUnitW = 44,
		kGroupPitch = 8,           // gap between a frame's last row and its border
		kGroupGap = 8,             // gap between two section frames
		kCaptionH = 20,            // room reserved for a section caption
		kMaxCards = 6,             // painted section frames
		kBtnW = 100,
		kBtnH = 26,

		// Frame heights derived from the row count, and the client height derived
		// from those. Hard-coding the client height once left the window 28px too
		// short, which clipped the action buttons off the bottom edge.
		kGroupH3 = kCaptionH + 2 * kRowH + kCtlH + kGroupPitch,
		kGroupH2 = kCaptionH + 1 * kRowH + kCtlH + kGroupPitch,
		kClientH = kPad
		         + kGroupH3 + kGroupGap   // 频率分析
		         + kGroupH3 + kGroupGap   // 显示
		         + kGroupH2 + kGroupGap   // 柱体动态
		         + kGroupH3 + 10          // 刻度
		         + kBtnH + kPad,          // action row
	};

	enum {
		// numeric / combo fields (same order as the tab order)
		id_bands = 2000, id_fft,
		id_style, id_gradmode,
		id_graddir, id_fps,
		id_alpha, id_gap,
		id_peakfall, id_gain, id_slope,
		id_freqmin, id_freqmax,
		id_bar_rise, id_bar_fall,
		// checkboxes
		id_db, id_log, id_grid, id_lowboost, id_scalefreq, id_scalelevel,
		// colour buttons
		id_color_base = 2100,
		// actions
		id_export = 2200, id_import, id_resetcolors, id_close,
	};

	static const unsigned kFftValues[] = { 2048, 4096, 8192, 16384 };

	//! Locale-independent, CRT-independent integer parse of an edit box.
	int wide_to_int(const wchar_t * s) {
		int v = 0;
		bool neg = false;
		if (*s == L'-') { neg = true; ++s; }
		while (*s >= L'0' && *s <= L'9') { v = v * 10 + (int)(*s - L'0'); ++s; }
		return neg ? -v : v;
	}

	class CSettingsWindow : public CWindowImpl<CSettingsWindow> {
	public:
		DECLARE_WND_CLASS_EX(TEXT("{7C4E1A93-5B2D-4F58-9A31-8D7E2C5B4F61}"),
			CS_HREDRAW | CS_VREDRAW, (HBRUSH)(COLOR_BTNFACE + 1));

		BEGIN_MSG_MAP_EX(CSettingsWindow)
			MSG_WM_CREATE(OnCreate)
			MSG_WM_DESTROY(OnDestroy)
			MSG_WM_CLOSE(OnClose)
			MSG_WM_PAINT(OnPaint)
			MSG_WM_ERASEBKGND(OnEraseBkgnd)
			MSG_WM_DRAWITEM(OnDrawItem)
			MSG_WM_COMMAND(OnCommand)
			MSG_WM_KEYDOWN(OnKeyDown)
			MSG_WM_MOUSEMOVE(OnMouseMove)
		END_MSG_MAP()

		~CSettingsWindow() {
			if (m_hWnd != NULL) DestroyWindow();
		}

		//! Creates the window. `owner` keeps it above the player window.
		bool create(HWND owner) {
			HDC screen = ::GetDC(NULL);
			m_dpi = (screen != NULL) ? ::GetDeviceCaps(screen, LOGPIXELSY) : 96;
			if (screen != NULL) ::ReleaseDC(NULL, screen);
			if (m_dpi <= 0) m_dpi = 96;

			CRect rc(0, 0, S(kClientW), S(kClientH));
			::AdjustWindowRectEx(&rc, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_TOOLWINDOW);

			if (Create(owner, rc, _T("频谱可视化设置"),
					WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
					WS_EX_TOOLWINDOW) == NULL) {
				return false;
			}
			center_on(owner);
			return true;
		}

		void bring_to_front() {
			if (m_hWnd == NULL) return;
			if (::IsIconic(m_hWnd)) ::ShowWindow(m_hWnd, SW_RESTORE);
			::SetForegroundWindow(m_hWnd);
		}

	private:
		int S(int px) const { return ::MulDiv(px, m_dpi, 96); }

		// --- control creation -------------------------------------------------

		HWND mk(const wchar_t * cls, const wchar_t * text, DWORD style, int x, int y, int w, int h, int id) {
			HWND c = ::CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
				S(x), S(y), S(w), S(h), m_hWnd, (HMENU)(INT_PTR)id, NULL, NULL);
			if (c != NULL && m_font.m_hFont != NULL) {
				::SendMessageW(c, WM_SETFONT, (WPARAM)m_font.m_hFont, TRUE);
			}
			return c;
		}

		HWND mk_label(const wchar_t * text, int x, int y, int w) {
			return mk(L"STATIC", text, SS_LEFT | SS_CENTERIMAGE, x, y, w, kCtlH, 0);
		}

		//! Section frames are painted rather than created as controls, so the
		//! corners can be rounded and the colours softened.
		void add_card(int x, int y, int w, int h, const wchar_t * caption) {
			if (m_cardCount >= kMaxCards) return;
			RECT rc = { S(x), S(y), S(x + w), S(y + h) };
			m_cards[m_cardCount].rc = rc;
			m_cards[m_cardCount].caption = caption;
			++m_cardCount;
		}

		HWND mk_edit(int id, int x, int y, int w) {
			return mk(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER, x, y, w, kCtlH, id);
		}

		//! Same as mk_edit() but without ES_NUMBER, so a minus sign can be typed.
		//! Used by the spectral tilt, which is the only signed field.
		HWND mk_edit_signed(int id, int x, int y, int w) {
			return mk(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, x, y, w, kCtlH, id);
		}

		HWND mk_combo(int id, int x, int y, int w) {
			return mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, x, y, w, kCtlH * 8, id);
		}

		HWND mk_check(int id, const wchar_t * text, int x, int y, int w) {
			return mk(L"BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP, x, y, w, kCtlH, id);
		}

		HWND mk_button(int id, const wchar_t * text, int x, int y, int w, int h, bool ownerdraw) {
			return mk(L"BUTTON", text, (ownerdraw ? BS_OWNERDRAW : BS_PUSHBUTTON) | WS_TABSTOP,
				x, y, w, h, id);
		}

		// --- tooltips ---------------------------------------------------------

		//! Tooltip text must outlive the tool, so every string is parked in a
		//! deque (stable element addresses) and pointed at from TOOLINFO.
		const wchar_t * keep_wide(const char * utf8) {
			pfc::stringcvt::string_os_from_utf8 wide(utf8);
			m_tipTexts.push_back(std::wstring(wide.get_ptr()));
			return m_tipTexts.back().c_str();
		}

		void add_tip(HWND ctrl, const char * utf8) {
			if (ctrl == NULL || m_tip.m_hWnd == NULL) return;
			TOOLINFOW ti = {};
			ti.cbSize = sizeof(ti);
			ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
			ti.hwnd = m_hWnd;
			ti.uId = (UINT_PTR)ctrl;
			ti.lpszText = (LPWSTR)keep_wide(utf8);
			::SendMessageW(m_tip.m_hWnd, TTM_ADDTOOLW, 0, (LPARAM)&ti);
		}

		// --- lifetime ---------------------------------------------------------

		int OnCreate(LPCREATESTRUCT) {
			create_font();
			spectrum_ui::apply_rounded_corners(m_hWnd);
			m_tip.Create(m_hWnd, (LPRECT)NULL, NULL, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX);
			if (m_tip.m_hWnd != NULL) {
				::SendMessageW(m_tip.m_hWnd, TTM_SETMAXTIPWIDTH, 0, S(340));
			}
			build_controls();
			load_from_settings();
			return 0;
		}

		void OnDestroy() {
			if (m_fontOwned && m_font.m_hFont != NULL) m_font.DeleteObject();
			m_font.m_hFont = NULL;
			g_instance = nullptr;
			SetMsgHandled(FALSE);
		}

		void OnClose() { DestroyWindow(); }
		void OnFinalMessage(HWND) override { delete this; }

		void create_font() {
			NONCLIENTMETRICSW ncm = { sizeof(ncm) };
			if (::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)
				&& m_font.CreateFontIndirectW(&ncm.lfMessageFont) != NULL) {
				m_fontOwned = true;
				return;
			}
			m_font.m_hFont = (HFONT)::GetStockObject(DEFAULT_GUI_FONT);
			m_fontOwned = false;
		}

		void center_on(HWND owner) {
			CRect mine, theirs;
			if (m_hWnd == NULL) return;
			GetWindowRect(&mine);
			if (owner != NULL && ::IsWindow(owner)) {
				::GetWindowRect(owner, &theirs);
			} else {
				theirs = CRect(0, 0, ::GetSystemMetrics(SM_CXSCREEN), ::GetSystemMetrics(SM_CYSCREEN));
			}
			const int x = theirs.left + ((theirs.Width() - mine.Width()) / 2);
			const int y = theirs.top + ((theirs.Height() - mine.Height()) / 2);
			::SetWindowPos(m_hWnd, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
		}

		// --- construction -----------------------------------------------------

		void build_controls() {
			// Note: no variable may be called ctl1/ctl2 here - dlgs.h (pulled in
			// by commdlg.h) defines those as control-id macros.
			// All coordinates below are 96-dpi units; mk*() applies the DPI scale.
			const int xlab1 = kPad + 12;
			const int xctl1 = xlab1 + kLabelW;
			const int xlab2 = kPad + 234;
			const int xctl2 = xlab2 + kLabelW;
			const int wNum = 42;
			const int xunit1 = xctl1 + wNum + 6;
			const int xunit2 = xctl2 + wNum + 6;

			// Frame heights come from the shared layout constants so the client
			// size and the content can never disagree.
			int gy = kPad;

			// ---- 频率分析 ---------------------------------------------------
			add_card(kPad, gy, kGrpW, kGroupH3, L"频率分析");
			{
				int y = gy + kCaptionH;
				add_tip(mk_label(L"波段数量", xlab1, y, kLabelW), "频谱分成多少根柱子。范围 64–1024，越大越细腻，但受面板像素宽度限制。");
				add_tip(mk_edit(id_bands, xctl1, y, kEditW), "频谱分成多少根柱子。范围 64–1024，越大越细腻，但受面板像素宽度限制。");
				add_tip(mk_label(L"FFT 点数", xlab2, y, kLabelW),
					"分析用的 FFT 长度。自动 = 按波段数决定（默认 4096）。越大低频越细腻，但时间窗越长、鼓点等瞬态越糊。");
				add_tip(mk_combo(id_fft, xctl2, y, kComboW),
					"分析用的 FFT 长度。自动 = 按波段数决定（默认 4096）。越大低频越细腻，但时间窗越长、鼓点等瞬态越糊。");

				y += kRowH;
				add_tip(mk_label(L"频率下限", xlab1, y, kLabelW), "只显示这个频率以上的频段，单位 Hz。");
				add_tip(mk_edit(id_freqmin, xctl1, y, wNum),
					"只显示这个频率以上的频段（20–20000 Hz）。\n"
					"选定的范围会自动放大铺满整个面板 —— 比如设成 20–200 Hz，256 根柱子就全部用来画低频，细节比全频段清楚得多。");
				mk_label(L"Hz", xunit1, y, kUnitW);
				add_tip(mk_label(L"频率上限", xlab2, y, kLabelW), "只显示这个频率以下的频段，单位 Hz。");
				add_tip(mk_edit(id_freqmax, xctl2, y, wNum),
					"只显示这个频率以下的频段（20–20000 Hz）。\n上限不能小于等于下限；若填反了会自动恢复成 20–20000。");
				mk_label(L"Hz", xunit2, y, kUnitW);

				y += kRowH;
				add_tip(mk_label(L"频谱倾斜", xlab1, y, kLabelW), "以 1 kHz 为轴心，每倍频程抬高或压低多少 dB。");
				add_tip(mk_edit_signed(id_slope, xctl1, y, wNum),
					"频谱倾斜，单位 dB/倍频程，以 1 kHz 为轴心（−12 ~ +12）。\n"
					"音乐能量天然每倍频程衰减约 6 dB，所以高频柱子天生就矮。填正值把高频抬起来，"
					"整条曲线就平了；填负值反过来强调低频。\n"
					"常用范围是 ±6：再大就会把某一段顶出显示范围。\n"
					"注意它和「增益」不同 —— 增益是全频段统一乘，改不了斜率。");
				mk_label(L"dB/oct", xunit1, y, kUnitW);
				add_tip(mk_check(id_lowboost, L"低频增强", xlab2, y, 180),
					"低于 500 Hz 的频段改用 16384 点长窗分析，高频仍用短窗。低频分辨率约提高 4 倍，而高频瞬态不受影响。关掉可省一次 FFT 计算。");
			}
			gy += kGroupH3 + kGroupGap;

			// ---- 显示 -------------------------------------------------------
			add_card(kPad, gy, kGrpW, kGroupH3, L"显示");
			{
				int y = gy + kCaptionH;
				add_tip(mk_label(L"显示样式", xlab1, y, kLabelW), "实心柱状 / 柱状+峰值保持 / 空心轮廓。");
				add_tip(mk_combo(id_style, xctl1, y, kComboW),
					"实心柱状：纯色柱体。\n柱状+峰值保持：每根柱子顶上另有一条缓慢回落的白线，标出近期峰值。\n空心轮廓：只描边不填充。");
				add_tip(mk_label(L"渐变模式", xlab2, y, kLabelW), "三色渐变 / 彩虹。");
				add_tip(mk_combo(id_gradmode, xctl2, y, kComboW),
					"三色渐变：用「底部 / 中间 / 顶部」三个颜色插值。\n彩虹：按 HSV 色相扫描生成，忽略三个颜色设置。");

				y += kRowH;
				add_tip(mk_label(L"渐变方向", xlab1, y, kLabelW), "颜色沿哪个方向变化。");
				add_tip(mk_combo(id_graddir, xctl1, y, kComboW),
					"水平：颜色随频率从左到右变化。\n垂直：颜色随高度从下到上变化（经典频谱观感，默认）。\n按电平：整根柱子一个颜色，随音量变化。");
				add_tip(mk_label(L"刷新率", xlab2, y, kLabelW), "每秒重绘次数，10–240。");
				add_tip(mk_edit(id_fps, xctl2, y, wNum),
					"每秒重绘次数，10–240。超过显示器刷新率没有意义；极高值会明显增加 CPU 占用。");
				mk_label(L"FPS", xunit2, y, kUnitW);

				y += kRowH;
				add_tip(mk_label(L"不透明度", xlab1, y, kLabelW), "柱体填充的不透明度，0–255。");
				add_tip(mk_edit(id_alpha, xctl1, y, kEditW),
					"柱体填充的不透明度：255 完全不透明，0 完全透明（只剩背景）。");
				add_tip(mk_label(L"柱间空隙", xlab2, y, kLabelW), "柱子之间留多少像素，0–8。");
				add_tip(mk_edit(id_gap, xctl2, y, wNum),
					"相邻柱子之间留多少像素，0 为紧贴。柱子很细时调大更容易分辨。");
				mk_label(L"px", xunit2, y, kUnitW);
			}
			gy += kGroupH3 + kGroupGap;

			// ---- 柱体动态 ---------------------------------------------------
			add_card(kPad, gy, kGrpW, kGroupH2, L"柱体动态");
			{
				int y = gy + kCaptionH;
				add_tip(mk_label(L"上升时间", xlab1, y, kLabelW), "柱子涨上去的速度。");
				add_tip(mk_edit(id_bar_rise, xctl1, y, wNum),
					"柱子跟随信号涨上去的时间（0–500 ms 时间常数）。\n"
					"0 = 立即跟上，瞬态最干脆（默认）；调大后上升变平滑，画面更柔和。\n"
					"时间常数指强度变化到约 63% 所需的时间。");
				mk_label(L"ms", xunit1, y, kUnitW);
				add_tip(mk_label(L"下落时间", xlab2, y, kLabelW), "柱子落回去的速度。");
				add_tip(mk_edit(id_bar_fall, xctl2, y, wNum),
					"柱子回落的时间常数（0–2000 ms）。\n"
					"越大落得越慢、拖尾越明显，画面更有「余韵」；0 = 信号一停立刻掉下去。\n"
					"默认 90 ms 约等于老版本的观感。此值按帧率换算，改刷新率不会改变视觉速度。");
				mk_label(L"ms", xunit2, y, kUnitW);

				y += kRowH;
				add_tip(mk_label(L"峰值下落", xlab1, y, kLabelW), "峰值标记下落速度，0–400 %/s。");
				add_tip(mk_edit(id_peakfall, xctl1, y, kEditW),
					"峰值保持标记的下落速度，单位是「满量程百分比/秒」。数值越大落得越快，0 则停在最高点不动。");
				add_tip(mk_label(L"增益", xlab2, y, kLabelW), "整体幅度放大倍数，1–400 %。");
				add_tip(mk_edit(id_gain, xctl2, y, wNum),
					"整体幅度放大倍数（1–400%）。信号偏小时调大，配合 dB 刻度效果更好。");
				mk_label(L"%", xunit2, y, kUnitW);
			}
			gy += kGroupH2 + kGroupGap;

			// ---- 刻度 -------------------------------------------------------
			add_card(kPad, gy, kGrpW, kGroupH3, L"刻度");
			{
				const int cbW = 128;
				const int cbX[3] = { xlab1, xlab1 + 134, xlab1 + 268 };
				int y = gy + kCaptionH;
				add_tip(mk_check(id_db, L"dB 刻度", cbX[0], y, cbW),
					"把幅度映射到 −60~0 dB 后再显示，小信号也能看清；关掉则按线性幅度显示。");
				add_tip(mk_check(id_log, L"对数频率轴", cbX[1], y, cbW),
					"频率轴按对数分布，更接近人耳听感，低频会占更多宽度；关掉则按线性分布。");
				add_tip(mk_check(id_grid, L"显示网格", cbX[2], y, cbW),
					"在绘图区叠加 7 条水平参考线，把纵向等分成 8 格。\n只画横线：竖线会和柱子叠在一起，反而让画面更乱。");

				y += kRowH;
				add_tip(mk_label(L"频率刻度", xlab1, y, kLabelW), "频率轴显示在绘图区哪一侧。");
				add_tip(mk_combo(id_scalefreq, xctl1, y, 110),
					"频率刻度画在绘图区的上方还是下方，或者不显示。\n刻度线、Hz 标签和「Hz」单位提示会一起改变位置。");

				y += kRowH;
				add_tip(mk_label(L"电平刻度", xlab1, y, kLabelW), "电平轴显示在绘图区哪一侧。");
				add_tip(mk_combo(id_scalelevel, xctl1, y, 110),
					"电平刻度画在绘图区的左侧还是右侧，或者不显示。\n刻度线与柱顶严格对齐，移到右侧后标签也跟着翻过去。");
			}
			gy += kGroupH3 + 10;

			// ---- actions ----------------------------------------------------
			mk_button(id_export, L"导出配置…", xlab1, gy, kBtnW, kBtnH, false);
			mk_button(id_import, L"导入配置…", xlab1 + kBtnW + 4, gy, kBtnW, kBtnH, false);
			mk_button(id_close, L"关闭", kPad + kGrpW - 96, gy, 96, kBtnH, false);
		}

		// --- control access ---------------------------------------------------

		int get_edit_int(int id) const {
			wchar_t buf[32] = {};
			::GetDlgItemTextW(m_hWnd, id, buf, 31);
			return wide_to_int(buf);
		}

		void set_edit_int(int id, int value) const {
			wchar_t buf[32];
			wsprintfW(buf, L"%d", value);
			::SetDlgItemTextW(m_hWnd, id, buf);
		}

		int get_combo_sel(int id) const {
			return (int)::SendDlgItemMessageW(m_hWnd, id, CB_GETCURSEL, 0, 0);
		}

		void set_combo_sel(int id, int sel) const {
			::SendDlgItemMessageW(m_hWnd, id, CB_SETCURSEL, (WPARAM)sel, 0);
		}

		void combo_add(int id, const wchar_t * text) const {
			::SendDlgItemMessageW(m_hWnd, id, CB_ADDSTRING, 0, (LPARAM)text);
		}

		bool is_checked(int id) const {
			return ::IsDlgButtonChecked(m_hWnd, id) == BST_CHECKED;
		}

		void set_checked(int id, bool on) const {
			::CheckDlgButton(m_hWnd, id, on ? BST_CHECKED : BST_UNCHECKED);
		}

		void fill_combos() {
			combo_add(id_style, L"实心柱状");
			combo_add(id_style, L"柱状 + 峰值保持");
			combo_add(id_style, L"空心轮廓");

			combo_add(id_gradmode, L"三色渐变");
			combo_add(id_gradmode, L"彩虹");

			combo_add(id_graddir, L"水平（按频率）");
			combo_add(id_graddir, L"垂直（下 → 上）");
			combo_add(id_graddir, L"按电平（整柱单色）");

			combo_add(id_fft, L"自动");
			for (unsigned i = 0; i < sizeof(kFftValues) / sizeof(kFftValues[0]); ++i) {
				wchar_t buf[16];
				wsprintfW(buf, L"%u", kFftValues[i]);
				combo_add(id_fft, buf);
			}

			// Index matches t_scale_pos: 0 = off, 1 = primary side, 2 = secondary.
			combo_add(id_scalefreq, L"不显示");
			combo_add(id_scalefreq, L"绘图区下方");
			combo_add(id_scalefreq, L"绘图区上方");

			combo_add(id_scalelevel, L"不显示");
			combo_add(id_scalelevel, L"绘图区左侧");
			combo_add(id_scalelevel, L"绘图区右侧");
		}

		// --- load / apply -----------------------------------------------------

		void load_from_settings() {
			const t_spectrum_settings s = spectrum_settings_load();

			fill_combos();

			set_edit_int(id_bands, (int)s.bands);
			set_edit_int(id_alpha, (int)s.alpha);
			set_edit_int(id_fps, (int)s.fps);
			set_edit_int(id_gap, (int)s.gap);
			set_edit_int(id_peakfall, (int)s.peak_fall);
			set_edit_int(id_gain, (int)s.gain);
			set_edit_int(id_slope, s.slope_db_per_oct);
			set_edit_int(id_freqmin, (int)s.freq_min);
			set_edit_int(id_freqmax, (int)s.freq_max);
			set_edit_int(id_bar_rise, (int)s.bar_rise_ms);
			set_edit_int(id_bar_fall, (int)s.bar_fall_ms);

			set_combo_sel(id_style, (int)s.style);
			set_combo_sel(id_gradmode, (int)s.gradient_mode);
			set_combo_sel(id_graddir, (int)s.gradient_dir);
			set_combo_sel(id_scalefreq, (int)s.scale_freq_pos);
			set_combo_sel(id_scalelevel, (int)s.scale_level_pos);

			int fftSel = 0;
			for (unsigned i = 0; i < sizeof(kFftValues) / sizeof(kFftValues[0]); ++i) {
				if (kFftValues[i] == s.fft) { fftSel = (int)i + 1; break; }
			}
			set_combo_sel(id_fft, fftSel);

			set_checked(id_db, s.db_scale);
			set_checked(id_log, s.log_freq);
			set_checked(id_grid, s.grid);
			set_checked(id_lowboost, s.low_boost);

			Invalidate(FALSE);
		}

		void apply_to_settings() {
			t_spectrum_settings s = spectrum_settings_load();

			s.bands = (unsigned)get_edit_int(id_bands);
			s.alpha = (unsigned)get_edit_int(id_alpha);
			s.fps = (unsigned)get_edit_int(id_fps);
			s.gap = (unsigned)get_edit_int(id_gap);
			s.peak_fall = (unsigned)get_edit_int(id_peakfall);
			s.gain = (unsigned)get_edit_int(id_gain);
			// Signed: the tilt can be negative.
			s.slope_db_per_oct = get_edit_int(id_slope);
			s.freq_min = (unsigned)get_edit_int(id_freqmin);
			s.freq_max = (unsigned)get_edit_int(id_freqmax);
			s.bar_rise_ms = (unsigned)get_edit_int(id_bar_rise);
			s.bar_fall_ms = (unsigned)get_edit_int(id_bar_fall);

			const int style = get_combo_sel(id_style);
			if (style >= 0) s.style = (unsigned)style;
			const int gmode = get_combo_sel(id_gradmode);
			if (gmode >= 0) s.gradient_mode = (unsigned)gmode;
			const int gdir = get_combo_sel(id_graddir);
			if (gdir >= 0) s.gradient_dir = (unsigned)gdir;
			const int sfp = get_combo_sel(id_scalefreq);
			if (sfp >= 0) s.scale_freq_pos = (unsigned)sfp;
			const int slp = get_combo_sel(id_scalelevel);
			if (slp >= 0) s.scale_level_pos = (unsigned)slp;

			const int fftSel = get_combo_sel(id_fft);
			s.fft = (fftSel > 0 && fftSel <= (int)(sizeof(kFftValues) / sizeof(kFftValues[0])))
				? kFftValues[fftSel - 1] : 0;

			s.db_scale = is_checked(id_db);
			s.log_freq = is_checked(id_log);
			s.grid = is_checked(id_grid);
			s.low_boost = is_checked(id_lowboost);

			spectrum_settings_save(s);
		}

		// --- commands ---------------------------------------------------------

		void OnCommand(UINT code, int id, CWindow) {
			if (id == id_close) { DestroyWindow(); return; }
			if (id == id_resetcolors) {
				spectrum_settings_apply_preset(0);
				load_from_settings();
				return;
			}
			if (id == id_export) { do_export(); return; }
			if (id == id_import) { do_import(); return; }

			if (id >= id_color_base && id < id_color_base + (int)spectrum_color_slot_count()) {
				pick_color((unsigned)(id - id_color_base));
				return;
			}

			// Edits commit when they lose focus so half-typed numbers never reach
			// the config; combos and checkboxes apply immediately.
			if (code == CBN_SELCHANGE || code == BN_CLICKED || code == EN_KILLFOCUS) {
				apply_to_settings();
			}
		}

		void OnKeyDown(UINT nChar, UINT, UINT) {
			if (nChar == VK_ESCAPE) DestroyWindow();
		}

		void pick_color(unsigned slot) {
			static COLORREF custom[16] = {};
			CHOOSECOLORW cc = {};
			cc.lStructSize = sizeof(cc);
			cc.hwndOwner = m_hWnd;
			cc.rgbResult = spectrum_color_slot_get(slot);
			cc.lpCustColors = custom;
			cc.Flags = CC_FULLOPEN | CC_RGBINIT | CC_ANYCOLOR;
			if (!::ChooseColorW(&cc)) return;
			spectrum_color_slot_set(slot, cc.rgbResult);
			Invalidate(FALSE);
		}

		void do_export() {
			const pfc::string8 text = spectrum_settings_serialize();

			wchar_t path[MAX_PATH] = L"foo_spectrum.ini";
			OPENFILENAMEW ofn = { sizeof(ofn) };
			ofn.hwndOwner = m_hWnd;
			ofn.lpstrFilter = L"配置文件 (*.ini)\0*.ini\0所有文件 (*.*)\0*.*\0\0";
			ofn.lpstrFile = path;
			ofn.nMaxFile = MAX_PATH;
			ofn.lpstrDefExt = L"ini";
			ofn.lpstrTitle = L"导出频谱可视化配置";
			ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;
			if (!::GetSaveFileNameW(&ofn)) return;

			HANDLE h = ::CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
				FILE_ATTRIBUTE_NORMAL, NULL);
			if (h == INVALID_HANDLE_VALUE) {
				::MessageBoxW(m_hWnd, L"无法写入文件。", L"导出配置", MB_ICONERROR | MB_OK);
				return;
			}
			// UTF-8 BOM so Notepad and friends show the Chinese comments correctly.
			static const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
			DWORD written = 0;
			::WriteFile(h, bom, 3, &written, NULL);
			::WriteFile(h, text.get_ptr(), (DWORD)text.length(), &written, NULL);
			::CloseHandle(h);
		}

		void do_import() {
			wchar_t path[MAX_PATH] = L"";
			OPENFILENAMEW ofn = { sizeof(ofn) };
			ofn.hwndOwner = m_hWnd;
			ofn.lpstrFilter = L"配置文件 (*.ini)\0*.ini\0所有文件 (*.*)\0*.*\0\0";
			ofn.lpstrFile = path;
			ofn.nMaxFile = MAX_PATH;
			ofn.lpstrDefExt = L"ini";
			ofn.lpstrTitle = L"导入频谱可视化配置";
			ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
			if (!::GetOpenFileNameW(&ofn)) return;

			HANDLE h = ::CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
				OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
			if (h == INVALID_HANDLE_VALUE) {
				::MessageBoxW(m_hWnd, L"无法打开文件。", L"导入配置", MB_ICONERROR | MB_OK);
				return;
			}
			LARGE_INTEGER size = {};
			if (!::GetFileSizeEx(h, &size) || size.QuadPart <= 0 || size.QuadPart > (4 << 20)) {
				::CloseHandle(h);
				::MessageBoxW(m_hWnd, L"文件为空或过大。", L"导入配置", MB_ICONERROR | MB_OK);
				return;
			}
			std::vector<char> buf((size_t)size.QuadPart + 1, 0);
			DWORD got = 0;
			const BOOL ok = ::ReadFile(h, buf.data(), (DWORD)size.QuadPart, &got, NULL);
			::CloseHandle(h);
			if (!ok) {
				::MessageBoxW(m_hWnd, L"读取文件失败。", L"导入配置", MB_ICONERROR | MB_OK);
				return;
			}
			buf[got] = 0;

			const char * text = buf.data();
			// tolerate a UTF-8 BOM
			if ((unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB
				&& (unsigned char)text[2] == 0xBF) {
				text += 3;
			}

			pfc::string8 error;
			if (!spectrum_settings_deserialize(text, error)) {
				pfc::stringcvt::string_os_from_utf8 wide(error.get_ptr());
				pfc::string8 msg;
				msg << "导入失败：" << error;
				pfc::stringcvt::string_os_from_utf8 wideMsg(msg.get_ptr());
				::MessageBoxW(m_hWnd, wideMsg.get_ptr(), L"导入配置", MB_ICONERROR | MB_OK);
				return;
			}
			load_from_settings();
			::MessageBoxW(m_hWnd, L"配置已导入并生效。", L"导入配置", MB_ICONINFORMATION | MB_OK);
		}

		// --- painting ---------------------------------------------------------

		BOOL OnEraseBkgnd(CDCHandle dc) {
			CRect rc;
			GetClientRect(&rc);
			dc.FillRect(&rc, (HBRUSH)(COLOR_BTNFACE + 1));
			return TRUE;
		}

		void OnPaint(CDCHandle) {
			CPaintDC dc(*this);
			CRect rc;
			if (!GetClientRect(&rc)) return;
			dc.FillRect(&rc, (HBRUSH)(COLOR_BTNFACE + 1));

			for (int i = 0; i < m_cardCount; ++i) {
				spectrum_ui::draw_section_frame(dc, m_cards[i].rc, m_cards[i].caption,
					m_font.m_hFont, S(7), S(11));
			}
		}

		void OnDrawItem(UINT, LPDRAWITEMSTRUCT dis) {
			if (dis == NULL) return;
			const int id = (int)dis->CtlID;
			if (id < id_color_base || id >= id_color_base + (int)spectrum_color_slot_count()) return;

			const unsigned slot = (unsigned)(id - id_color_base);
			const COLORREF color = spectrum_color_slot_get(slot);

			CDC dc;
			dc.Attach(dis->hDC);
			CRect rc = dis->rcItem;

			HBRUSH fill = ::CreateSolidBrush(color);
			if (fill != NULL) {
				::FillRect(dc, &rc, fill);
				::DeleteObject(fill);
			}

			const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
			dc.Draw3dRect(&rc, ::GetSysColor(pressed ? COLOR_3DSHADOW : COLOR_3DHILIGHT),
				::GetSysColor(pressed ? COLOR_3DHILIGHT : COLOR_3DSHADOW));

			const int luma = GetRValue(color) + GetGValue(color) + GetBValue(color);
			wchar_t hex[16];
			wsprintfW(hex, L"%02X%02X%02X", GetRValue(color), GetGValue(color), GetBValue(color));
			dc.SetBkMode(TRANSPARENT);
			dc.SetTextColor(luma > 384 ? RGB(0, 0, 0) : RGB(255, 255, 255));
			HFONT oldFont = NULL;
			if (m_font.m_hFont != NULL) oldFont = dc.SelectFont(m_font.m_hFont);
			dc.DrawText(hex, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
			if (oldFont != NULL) dc.SelectFont(oldFont);

			dc.Detach();
		}

		void OnMouseMove(UINT, CPoint) {
			// TTF_SUBCLASS on every tool already makes hovering work; nothing to do.
		}

	public:
		//! Live instance, so a second request just raises the existing window.
		static CSettingsWindow * g_instance;

	private:
		struct t_card {
			RECT rc;
			const wchar_t * caption;
		};

		int m_dpi = 96;
		CFont m_font;
		bool m_fontOwned = false;
		CToolTipCtrl m_tip;
		std::deque<std::wstring> m_tipTexts;
		t_card m_cards[kMaxCards];
		int m_cardCount = 0;
	};

	CSettingsWindow * CSettingsWindow::g_instance = nullptr;

} // anonymous namespace

void spectrum_open_settings_window(HWND owner) {
	if (CSettingsWindow::g_instance != nullptr) {
		CSettingsWindow::g_instance->bring_to_front();
		return;
	}

	CSettingsWindow * w = new CSettingsWindow();
	if (!w->create(owner)) {
		delete w;
		return;
	}
	CSettingsWindow::g_instance = w;
}
