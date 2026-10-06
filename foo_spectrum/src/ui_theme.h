#pragma once

// ---------------------------------------------------------------------------
// Small drawing helpers shared by the component's settings windows.
//
// Both windows are assembled from stock controls, so every "modern" touch has to
// be painted by hand: rounded window corners through DWM, and rounded section
// frames drawn on the dialog background instead of square BS_GROUPBOX controls.
//
// Header-only and inline on purpose - no extra translation unit, and the linker
// drops whatever a given window does not use.
// ---------------------------------------------------------------------------

namespace spectrum_ui {

	//! Linear colour blend: 0 -> a, 100 -> b.
	inline COLORREF blend(COLORREF a, COLORREF b, int percentB) {
		if (percentB < 0) percentB = 0;
		if (percentB > 100) percentB = 100;
		return RGB(
			(GetRValue(a) * (100 - percentB) + GetRValue(b) * percentB) / 100,
			(GetGValue(a) * (100 - percentB) + GetGValue(b) * percentB) / 100,
			(GetBValue(a) * (100 - percentB) + GetBValue(b) * percentB) / 100);
	}

	//! Asks DWM for rounded window corners. Windows 11 honours it; on Windows 10
	//! the call simply fails and the window keeps its square corners.
	inline void apply_rounded_corners(HWND hwnd) {
		if (hwnd == NULL) return;

		// DWMWA_WINDOW_CORNER_PREFERENCE = 33, DWMWCP_ROUND = 2.
		// dwmapi.dll is resolved on demand so the component keeps no import on it.
		typedef HRESULT (WINAPI * tDwmSetWindowAttribute)(HWND, DWORD, LPCVOID, DWORD);
		HMODULE dwm = ::LoadLibraryW(L"dwmapi.dll");
		if (dwm == NULL) return;

		tDwmSetWindowAttribute setAttribute =
			reinterpret_cast<tDwmSetWindowAttribute>(::GetProcAddress(dwm, "DwmSetWindowAttribute"));
		if (setAttribute != NULL) {
			const DWORD preference = 2; // DWMWCP_ROUND
			setAttribute(hwnd, 33, &preference, sizeof(preference));
		}
		::FreeLibrary(dwm);
	}

	//! Fills a rounded rectangle, falling back to a plain fill if the region
	//! cannot be created.
	inline void fill_rounded(HDC dc, const RECT & rc, int radius, COLORREF color) {
		HBRUSH brush = ::CreateSolidBrush(color);
		if (brush == NULL) return;

		HRGN rgn = ::CreateRoundRectRgn(rc.left, rc.top, rc.right + 1, rc.bottom + 1,
			radius * 2, radius * 2);
		if (rgn == NULL) {
			::FillRect(dc, &rc, brush);
		} else {
			::FillRgn(dc, rgn, brush);
			::DeleteObject(rgn);
		}
		::DeleteObject(brush);
	}

	//! Outlines a rounded rectangle.
	inline void stroke_rounded(HDC dc, const RECT & rc, int radius, COLORREF color) {
		HPEN pen = ::CreatePen(PS_SOLID, 1, color);
		if (pen == NULL) return;

		HGDIOBJ oldPen = ::SelectObject(dc, pen);
		HGDIOBJ oldBrush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
		::RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, radius * 2, radius * 2);
		::SelectObject(dc, oldBrush);
		::SelectObject(dc, oldPen);
		::DeleteObject(pen);
	}

	//! Rounded section frame whose caption sits on the top border, the way a
	//! classic group box does - just rounder and in a softer colour.
	//!
	//! The frame is deliberately drawn on the plain dialog background: standard
	//! buttons and checkboxes paint their own COLOR_BTNFACE face, so a tinted card
	//! behind them would show up as grey blocks around every control.
	inline void draw_section_frame(HDC dc, const RECT & rc, const wchar_t * caption,
	                               HFONT font, int radius, int captionPadX) {
		const COLORREF page = ::GetSysColor(COLOR_BTNFACE);
		stroke_rounded(dc, rc, radius, blend(page, ::GetSysColor(COLOR_3DSHADOW), 55));

		if (caption == NULL || *caption == 0) return;

		HFONT oldFont = NULL;
		if (font != NULL) oldFont = (HFONT)::SelectObject(dc, font);

		const int len = ::lstrlenW(caption);
		SIZE sz = {};
		::GetTextExtentPoint32W(dc, caption, len, &sz);

		const int tx = rc.left + captionPadX;
		const int ty = rc.top - sz.cy / 2;

		// Punch a hole in the top border so the caption reads as a legend.
		RECT gap = { tx - 4, rc.top - 1, tx + sz.cx + 5, rc.top + 2 };
		HBRUSH bg = ::CreateSolidBrush(page);
		if (bg != NULL) {
			::FillRect(dc, &gap, bg);
			::DeleteObject(bg);
		}

		::SetBkMode(dc, TRANSPARENT);
		::SetTextColor(dc, blend(page, ::GetSysColor(COLOR_BTNTEXT), 80));
		::TextOutW(dc, tx, ty, caption, len);

		if (oldFont != NULL) ::SelectObject(dc, oldFont);
	}

} // namespace spectrum_ui
