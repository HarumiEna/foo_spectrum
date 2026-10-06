#include "stdafx.h"
#include "settings.h"
#include "color_dialog.h"
#include "screen_picker.h"
#include "ui_theme.h"

#include <commdlg.h>

#include <deque>
#include <string>

// ---------------------------------------------------------------------------
// Colour window.
//
// Built programmatically like the main settings window (see settings_dialog.cpp
// for the rationale). Layout is in 96-dpi units and scaled at creation time.
// Controls are grouped with BS_GROUPBOX frames so the window reads as sections
// rather than one flat grid.
// ---------------------------------------------------------------------------

namespace {

	enum {
		kPad = 12,
		kGrpW = 428,          // section frame width
		kClientW = kPad * 2 + kGrpW,
		kCtlH = 22,
		kRowH = 28,
		kLabelW = 78,
		kSwatchW = 68,
		kSwatchH = 26,
		kListW = 210,
		kListH = 118,
		kCaptionH = 20,
		kGroupPitch = 8,      // gap between a frame's last row and its border
		kGroupGap = 8,        // gap between two section frames
		kMaxCards = 4,        // painted section frames
		kBtnH = 26,
		kSwatchRows = 4,
		kSwatchRowY = 30,     // first swatch row, measured from the frame top

		// Frames and the client height are all derived from the layout, so the
		// content and the window size cannot drift apart.
		kColorGroupH = kSwatchRowY + (kSwatchRows - 1) * kRowH + kSwatchH + kGroupPitch,
		kPickGroupY = kPad + kColorGroupH + kGroupGap,
		kPickGroupH = kCaptionH + kCtlH + kGroupPitch,
		kPresetGroupY = kPickGroupY + kPickGroupH + kGroupGap,
		kPresetGroupH = kCaptionH + kListH + kGroupPitch,
		kClientH = kPresetGroupY + kPresetGroupH + 10 + kBtnH + kPad,
	};

	enum {
		id_color_base = 2100,
		id_mode_dialog = 2200, id_mode_eyedrop,
		id_presets = 2210, id_presetname = 2211,
		id_apply = 2220, id_save, id_delete, id_close,
	};

	class CColorWindow : public CWindowImpl<CColorWindow> {
	public:
		DECLARE_WND_CLASS_EX(TEXT("{7C4E1A93-5B2D-4F58-9A31-8D7E2C5B4F62}"),
			CS_HREDRAW | CS_VREDRAW, (HBRUSH)(COLOR_BTNFACE + 1));

		BEGIN_MSG_MAP_EX(CColorWindow)
			MSG_WM_CREATE(OnCreate)
			MSG_WM_DESTROY(OnDestroy)
			MSG_WM_CLOSE(OnClose)
			MSG_WM_PAINT(OnPaint)
			MSG_WM_ERASEBKGND(OnEraseBkgnd)
			MSG_WM_DRAWITEM(OnDrawItem)
			MSG_WM_COMMAND(OnCommand)
			MSG_WM_KEYDOWN(OnKeyDown)
		END_MSG_MAP()

		~CColorWindow() {
			if (m_hWnd != NULL) DestroyWindow();
		}

		bool create(HWND owner) {
			HDC screen = ::GetDC(NULL);
			m_dpi = (screen != NULL) ? ::GetDeviceCaps(screen, LOGPIXELSY) : 96;
			if (screen != NULL) ::ReleaseDC(NULL, screen);
			if (m_dpi <= 0) m_dpi = 96;

			CRect rc(0, 0, S(kClientW), S(kClientH));
			::AdjustWindowRectEx(&rc, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_TOOLWINDOW);
			if (Create(owner, rc, _T("颜色设置"),
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

		static CColorWindow * g_instance;

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

		HWND mk_button(int id, const wchar_t * text, int x, int y, int w, int h, bool ownerdraw) {
			return mk(L"BUTTON", text, (ownerdraw ? BS_OWNERDRAW : BS_PUSHBUTTON) | WS_TABSTOP,
				x, y, w, h, id);
		}

		HWND mk_radio(int id, const wchar_t * text, int x, int y, int w, bool first) {
			return mk(L"BUTTON", text,
				BS_AUTORADIOBUTTON | WS_TABSTOP | (first ? WS_GROUP : 0), x, y, w, kCtlH, id);
		}

		// --- tooltips ---------------------------------------------------------

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

		void tip_for_swatch(HWND ctrl, const char * name) {
			pfc::string8 tip;
			tip << name
			    << "：左键点击按当前选中的取色方式修改。\n"
			       "选「标准取色器」会弹出系统调色板；选「屏幕吸管」则出现放大镜，可从屏幕任意位置吸色。";
			add_tip(ctrl, tip.get_ptr());
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
			refresh_presets();
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
			if (m_hWnd == NULL) return;
			CRect mine, theirs;
			GetWindowRect(&mine);
			if (owner != NULL && ::IsWindow(owner)) {
				::GetWindowRect(owner, &theirs);
			} else {
				theirs = CRect(0, 0, ::GetSystemMetrics(SM_CXSCREEN), ::GetSystemMetrics(SM_CYSCREEN));
			}
			::SetWindowPos(m_hWnd, NULL,
				theirs.left + (theirs.Width() - mine.Width()) / 2,
				theirs.top + (theirs.Height() - mine.Height()) / 2,
				0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
		}

		// --- construction -----------------------------------------------------

		void build_controls() {
			build_color_group();
			build_pick_group();
			build_preset_group();
			mk_button(id_close, L"关闭", kPad + kGrpW - 96, kClientH - kPad - kBtnH, 96, kBtnH, false);
		}

		void build_color_group() {
			// Four rows in two columns: gradient stops down the left, the rest on
			// the right. Each swatch carries its own name so nothing has to be
			// looked up in a menu.
			add_card(kPad, kPad, kGrpW, kColorGroupH, L"颜色（左键点击色块修改）");

			const unsigned slots = spectrum_color_slot_count();
			for (unsigned i = 0; i < slots; ++i) {
				const int col = (int)(i / 4);
				const int row = (int)(i % 4);
				const int lx = kPad + 12 + col * 208;
				const int sx = lx + kLabelW;
				const int y = kPad + kSwatchRowY + row * kRowH;

				pfc::stringcvt::string_os_from_utf8 wide(spectrum_color_slot_name(i));
				HWND label = mk_label(wide.get_ptr(), lx, y, kLabelW);
				add_tip(label, "该颜色在频谱上的作用位置。");

				HWND b = mk_button((int)(id_color_base + i), L"", sx, y - 2, kSwatchW, kSwatchH, true);
				tip_for_swatch(b, spectrum_color_slot_name(i));
			}
		}

		void build_pick_group() {
			const int gy = kPickGroupY;
			add_card(kPad, gy, kGrpW, kPickGroupH, L"取色方式");

			const int y = gy + kCaptionH;
			add_tip(mk_radio(id_mode_dialog, L"标准取色器", kPad + 12, y, 112, true),
				"弹出 Windows 标准颜色对话框：完整色板、色相/饱和度/亮度调节、十六进制输入框。适合精确微调。");
			add_tip(mk_radio(id_mode_eyedrop, L"屏幕吸管", kPad + 140, y, 112, false),
				"弹出跟随鼠标的放大镜，可从屏幕任意位置吸取颜色（多显示器可用）。适合照着别的画面配色。");
			::CheckRadioButton(m_hWnd, id_mode_dialog, id_mode_eyedrop, id_mode_dialog);
		}

		void build_preset_group() {
			const int gy = kPresetGroupY;
			const int listY = gy + kCaptionH;
			add_card(kPad, gy, kGrpW, kPresetGroupH, L"配色预设（内置在前，自定义在后）");

			HWND list = mk(L"LISTBOX", L"",
				WS_BORDER | WS_VSCROLL | LBS_NOTIFY | WS_TABSTOP,
				kPad + 12, listY, kListW, kListH, id_presets);
			add_tip(list, "内置预设来自程序，自定义预设是你自己保存的配色。\n双击一项即可套用；选中后也可以用右侧的「应用所选」。");

			const int rx = kPad + 234;
			const int rw = kGrpW - 234 - 12;

			mk_label(L"预设名称", rx, listY, 88);
			HWND nameEdit = mk(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL,
				rx + 92, listY, rw - 92, kCtlH, id_presetname);
			add_tip(nameEdit, "保存自定义预设时用的名字。留空会自动取一个（如「配色 1」）；同名会覆盖原有预设。");

			const int btnY = listY + kCtlH + 10;
			add_tip(mk_button(id_apply, L"应用所选", rx, btnY, 96, kBtnH, false),
				"把列表中选中的预设套用到频谱面板上。");
			add_tip(mk_button(id_delete, L"删除", rx + rw - 96, btnY, 96, kBtnH, false),
				"删除选中的自定义预设。内置预设不能删除。");

			add_tip(mk_button(id_save, L"保存当前配色为预设", rx, btnY + kBtnH + 8, rw, kBtnH, false),
				"把频谱面板当前正在使用的配色，连同渐变模式与方向，保存成一个自定义预设（最多 32 个）。");
		}

		// --- preset list ------------------------------------------------------

		void refresh_presets() {
			HWND list = ::GetDlgItem(m_hWnd, id_presets);
			if (list == NULL) return;

			const int keep = (int)::SendMessageW(list, LB_GETCURSEL, 0, 0);
			::SendMessageW(list, LB_RESETCONTENT, 0, 0);

			const unsigned builtin = spectrum_settings_preset_count();
			for (unsigned i = 0; i < builtin; ++i) {
				pfc::string8 text;
				text << "内置 · " << spectrum_settings_preset_name(i);
				pfc::stringcvt::string_os_from_utf8 wide(text.get_ptr());
				::SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)wide.get_ptr());
			}

			const unsigned user = spectrum_user_preset_count();
			for (unsigned i = 0; i < user; ++i) {
				pfc::string8 text;
				text << "自定义 · " << spectrum_user_preset_name(i);
				pfc::stringcvt::string_os_from_utf8 wide(text.get_ptr());
				::SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)wide.get_ptr());
			}

			const int count = (int)::SendMessageW(list, LB_GETCOUNT, 0, 0);
			if (keep >= 0 && keep < count) {
				::SendMessageW(list, LB_SETCURSEL, (WPARAM)keep, 0);
			} else if (count > 0) {
				::SendMessageW(list, LB_SETCURSEL, 0, 0);
			}
		}

		int selected_preset(int & builtinCount, bool & isUser) const {
			HWND list = ::GetDlgItem(m_hWnd, id_presets);
			builtinCount = (int)spectrum_settings_preset_count();
			isUser = false;
			if (list == NULL) return -1;
			const int sel = (int)::SendMessageW(list, LB_GETCURSEL, 0, 0);
			if (sel < 0) return -1;
			isUser = (sel >= builtinCount);
			return sel;
		}

		void apply_selected() {
			int builtinCount = 0;
			bool isUser = false;
			const int sel = selected_preset(builtinCount, isUser);
			if (sel < 0) return;

			if (isUser) spectrum_user_preset_apply((unsigned)(sel - builtinCount));
			else        spectrum_settings_apply_preset((unsigned)sel);

			Invalidate(FALSE);
		}

		void save_current() {
			wchar_t buf[128] = {};
			::GetDlgItemTextW(m_hWnd, id_presetname, buf, 127);
			if (buf[0] == 0) {
				// No name given: fall back to the next default one.
				wsprintfW(buf, L"配色 %u", spectrum_user_preset_count() + 1);
			}

			pfc::stringcvt::string_utf8_from_os name(buf);
			if (!spectrum_user_preset_save(name.get_ptr())) {
				::MessageBoxW(m_hWnd,
					L"保存失败：名称是空的，或者自定义预设已达上限（32 个）。",
					L"颜色设置", MB_ICONWARNING | MB_OK);
				return;
			}
			// reflect the (possibly auto-generated) name back into the box
			::SetDlgItemTextW(m_hWnd, id_presetname, buf);
			refresh_presets();
			Invalidate(FALSE);
		}

		void delete_selected() {
			int builtinCount = 0;
			bool isUser = false;
			const int sel = selected_preset(builtinCount, isUser);
			if (sel < 0) return;
			if (!isUser) {
				::MessageBoxW(m_hWnd, L"内置预设不能删除。", L"颜色设置", MB_ICONINFORMATION | MB_OK);
				return;
			}
			spectrum_user_preset_delete((unsigned)(sel - builtinCount));
			refresh_presets();
		}

		// --- interaction ------------------------------------------------------

		void OnCommand(UINT code, int id, CWindow) {
			if (id == id_close) { DestroyWindow(); return; }
			if (id == id_apply) { apply_selected(); return; }
			if (id == id_save) { save_current(); return; }
			if (id == id_delete) { delete_selected(); return; }

			if (id >= id_color_base && id < id_color_base + (int)spectrum_color_slot_count()) {
				pick_color((unsigned)(id - id_color_base));
				return;
			}

			if (id == id_presets && code == LBN_DBLCLK) {
				apply_selected();
			}
		}

		void OnKeyDown(UINT nChar, UINT, UINT) {
			if (nChar == VK_ESCAPE) DestroyWindow();
		}

		//! Left click uses whichever picker the radio pair selects.
		void pick_color(unsigned slot) {
			const bool useEyedropper =
				(::IsDlgButtonChecked(m_hWnd, id_mode_eyedrop) == BST_CHECKED);

			if (useEyedropper) {
				COLORREF picked = spectrum_color_slot_get(slot);
				if (spectrum_pick_color_from_screen(m_hWnd, spectrum_color_slot_name(slot), picked)) {
					spectrum_color_slot_set(slot, picked);
				}
			} else {
				static COLORREF custom[16] = {};
				CHOOSECOLORW cc = {};
				cc.lStructSize = sizeof(cc);
				cc.hwndOwner = m_hWnd;
				cc.rgbResult = spectrum_color_slot_get(slot);
				cc.lpCustColors = custom;
				cc.Flags = CC_FULLOPEN | CC_RGBINIT | CC_ANYCOLOR;
				if (!::ChooseColorW(&cc)) return;
				spectrum_color_slot_set(slot, cc.rgbResult);
			}
			Invalidate(FALSE);
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

			const int radius = S(5);
			spectrum_ui::fill_rounded(dc, rc, radius, color);

			const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
			const COLORREF borderColor = pressed
				? ::GetSysColor(COLOR_3DSHADOW)
				: spectrum_ui::blend(::GetSysColor(COLOR_BTNFACE),
					::GetSysColor(COLOR_3DSHADOW), 70);
			spectrum_ui::stroke_rounded(dc, rc, radius, borderColor);

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

	CColorWindow * CColorWindow::g_instance = nullptr;

} // anonymous namespace

void spectrum_open_color_window(HWND owner) {
	if (CColorWindow::g_instance != nullptr) {
		CColorWindow::g_instance->bring_to_front();
		return;
	}

	CColorWindow * w = new CColorWindow();
	if (!w->create(owner)) {
		delete w;
		return;
	}
	CColorWindow::g_instance = w;
}
