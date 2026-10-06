#include "stdafx.h"
#include "screen_picker.h"

// ---------------------------------------------------------------------------
// Screen eyedropper.
//
// Design notes:
//  * A small topmost popup ("loupe") follows the cursor instead of a full
//    screen overlay. A full screen overlay would have to be either opaque
//    (hiding the very thing you are aiming at) or colour-keyed, and colour-keyed
//    layered windows are click-through exactly where they are transparent -
//    which would swallow the click that is supposed to take the sample.
//  * Screen pixels are read through a DISPLAY DC, whose coordinate space is the
//    virtual desktop, so multi-monitor setups (including negative coordinates)
//    work without extra mapping.
//  * Mouse input is taken with SetCapture, so the loupe receives moves and the
//    final click no matter where the cursor is.
// ---------------------------------------------------------------------------

namespace {

	enum {
		kSampleRadius = 7,      // -> 15x15 screen pixels magnified
		kCellBase     = 12,     // magnified size of one screen pixel at 96 dpi
		kInfoBase     = 26,     // height of the colour/hex strip at 96 dpi
		kGap          = 22,     // distance between cursor and loupe
	};

	class CScreenPickerWnd : public CWindowImpl<CScreenPickerWnd> {
	public:
		DECLARE_WND_CLASS_EX(TEXT("{4E1A7C93-6B2D-4F58-9A31-8D7E2C5B4F60}"),
			CS_HREDRAW | CS_VREDRAW, (-1));

		BEGIN_MSG_MAP_EX(CScreenPickerWnd)
			MSG_WM_CREATE(OnCreate)
			MSG_WM_DESTROY(OnDestroy)
			MSG_WM_PAINT(OnPaint)
			MSG_WM_ERASEBKGND(OnEraseBkgnd)
			MSG_WM_MOUSEMOVE(OnMouseMove)
			MSG_WM_LBUTTONDOWN(OnLButtonDown)
			MSG_WM_RBUTTONDOWN(OnRButtonDown)
			MSG_WM_MBUTTONDOWN(OnRButtonDown)
			MSG_WM_KEYDOWN(OnKeyDown)
			MSG_WM_SETCURSOR(OnSetCursor)
			MSG_WM_CAPTURECHANGED(OnCaptureChanged)
		END_MSG_MAP()

		~CScreenPickerWnd() {
			if (m_hWnd != NULL) DestroyWindow();
		}

		bool create(HWND owner, const char * label) {
			m_label = label;

			// One cell of the magnifier per screen pixel; double it on high DPI
			// so the loupe stays comfortably readable.
			HDC screen = ::GetDC(NULL);
			if (screen != NULL) {
				const int dpi = ::GetDeviceCaps(screen, LOGPIXELSY);
				::ReleaseDC(NULL, screen);
				if (dpi >= 144) m_scale = 2;
			}
			m_cell = kCellBase * m_scale;
			m_side = (kSampleRadius * 2 + 1) * m_cell;
			m_infoH = kInfoBase * m_scale;

			// _U_RECT only binds to an lvalue RECT, hence the named variable.
			CRect rc(0, 0, m_side, m_side + m_infoH);
			return Create(owner, rc, NULL,
				WS_POPUP | WS_VISIBLE, WS_EX_TOPMOST | WS_EX_TOOLWINDOW) != NULL;
		}

		void begin(POINT pt) {
			move_to(pt);
			::SetCapture(m_hWnd);
			InvalidateRect(NULL, FALSE);
		}

		bool done() const { return m_done; }
		bool accepted() const { return m_accepted; }
		COLORREF color() const { return m_color; }
		HWND hwnd() { return m_hWnd; }
		void force_destroy() { if (m_hWnd != NULL) DestroyWindow(); }

	private:
		// --- window lifetime -------------------------------------------------

		int OnCreate(LPCREATESTRUCT) {
			m_screenDC = ::CreateDC(_T("DISPLAY"), NULL, NULL, NULL);
			HDC ref = (m_screenDC != NULL) ? m_screenDC : ::GetDC(NULL);
			m_sampleDC = ::CreateCompatibleDC(ref);
			if (m_sampleDC != NULL) {
				const int side = kSampleRadius * 2 + 1;
				// Note: the bitmap must be created from the screen DC, not from
				// the memory DC - the memory DC still has its 1x1 monochrome
				// default bitmap selected, which would yield a mono bitmap.
				m_sampleBmp = ::CreateCompatibleBitmap(ref, side, side);
				if (m_sampleBmp != NULL) m_sampleOld = ::SelectObject(m_sampleDC, m_sampleBmp);
			}
			if (m_screenDC == NULL) ::ReleaseDC(NULL, ref);
			return 0;
		}

		void OnDestroy() {
			if (m_sampleDC != NULL && m_sampleOld != NULL) ::SelectObject(m_sampleDC, m_sampleOld);
			m_sampleOld = NULL;
			if (m_sampleBmp != NULL) { ::DeleteObject(m_sampleBmp); m_sampleBmp = NULL; }
			if (m_sampleDC != NULL) { ::DeleteDC(m_sampleDC); m_sampleDC = NULL; }
			if (m_screenDC != NULL) { ::DeleteDC(m_screenDC); m_screenDC = NULL; }
			SetMsgHandled(FALSE);
		}

		void finish(bool accept) {
			if (m_done) return;
			m_done = true;
			m_accepted = accept;
			if (::GetCapture() == m_hWnd) ::ReleaseCapture();
			if (m_hWnd != NULL) DestroyWindow();
		}

		// --- interaction -----------------------------------------------------

		void move_to(POINT pt) {
			m_pt = pt;

			if (m_screenDC != NULL) m_color = ::GetPixel(m_screenDC, pt.x, pt.y);

			// Park the loupe diagonally next to the cursor, flipping to the other
			// side near a monitor edge. This keeps the cursor outside the loupe,
			// so the sampled pixel is never one of the loupe's own pixels.
			MONITORINFO mi = { sizeof(mi) };
			::GetMonitorInfoW(::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);

			const int fullH = m_side + m_infoH;
			int x = pt.x + kGap;
			int y = pt.y + kGap;
			if (x + m_side > mi.rcMonitor.right) x = pt.x - kGap - m_side;
			if (y + fullH > mi.rcMonitor.bottom) y = pt.y - kGap - fullH;
			if (x < mi.rcMonitor.left) x = mi.rcMonitor.left;
			if (y < mi.rcMonitor.top) y = mi.rcMonitor.top;

			::SetWindowPos(m_hWnd, HWND_TOPMOST, x, y, 0, 0,
				SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
		}

		void OnMouseMove(UINT, CPoint) {
			if ((::GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0) { finish(false); return; }
			POINT pt;
			::GetCursorPos(&pt);
			move_to(pt);
			InvalidateRect(NULL, FALSE);
		}

		void OnLButtonDown(UINT, CPoint) { finish(true); }
		void OnRButtonDown(UINT, CPoint) { finish(false); }
		void OnKeyDown(UINT nChar, UINT, UINT) { if (nChar == VK_ESCAPE) finish(false); }
		void OnCaptureChanged(CWindow) { if (!m_done) finish(false); }

		BOOL OnSetCursor(CWindow, UINT, UINT) {
			::SetCursor(::LoadCursor(NULL, IDC_CROSS));
			return TRUE;
		}

		// --- painting --------------------------------------------------------

		BOOL OnEraseBkgnd(CDCHandle) { return TRUE; }

		void OnPaint(CDCHandle) {
			CPaintDC dc(*this);
			CRect rc;
			if (!GetClientRect(&rc)) return;

			const int side = kSampleRadius * 2 + 1;

			// Magnifier: nearest-neighbour upscale so every screen pixel shows up
			// as a crisp cell (COLORONCOLOR = no interpolation).
			if (m_sampleDC != NULL && m_screenDC != NULL) {
				::BitBlt(m_sampleDC, 0, 0, side, side, m_screenDC,
					m_pt.x - kSampleRadius, m_pt.y - kSampleRadius, SRCCOPY);
			}
			::SetStretchBltMode(dc, COLORONCOLOR);
			::StretchBlt(dc, 0, 0, m_side, m_side, m_sampleDC, 0, 0, side, side, SRCCOPY);

			// Crosshair: outline the exact source pixel in black then white, so it
			// stays visible on any background.
			CRect centre(kSampleRadius * m_cell, kSampleRadius * m_cell,
				(kSampleRadius + 1) * m_cell, (kSampleRadius + 1) * m_cell);
			CRect outer = centre;
			outer.InflateRect(1, 1);
			::FrameRect(dc, &outer, (HBRUSH)::GetStockObject(BLACK_BRUSH));
			::FrameRect(dc, &centre, (HBRUSH)::GetStockObject(WHITE_BRUSH));

			// Info strip: the colour itself as a swatch, plus its value.
			CRect info(0, m_side, rc.right, rc.bottom);
			HBRUSH swatch = ::CreateSolidBrush(m_color);
			if (swatch != NULL) { ::FillRect(dc, &info, swatch); ::DeleteObject(swatch); }

			const int luma = GetRValue(m_color) + GetGValue(m_color) + GetBValue(m_color);
			const COLORREF textCol = (luma > 384) ? RGB(0, 0, 0) : RGB(255, 255, 255);

			char hex[16];
			wsprintfA(hex, "#%02X%02X%02X",
				GetRValue(m_color), GetGValue(m_color), GetBValue(m_color));

			pfc::string8 text;
			if (m_label != NULL && *m_label != 0) text << m_label << "   ";
			text << hex;

			HGDIOBJ oldFont = ::SelectObject(dc, ::GetStockObject(DEFAULT_GUI_FONT));
			const int oldBk = ::SetBkMode(dc, TRANSPARENT);
			const COLORREF oldText = ::SetTextColor(dc, textCol);
			pfc::stringcvt::string_os_from_utf8 wide(text.get_ptr());
			CRect textRc = info;
			textRc.DeflateRect(4 * m_scale, 0);
			::DrawTextW(dc, wide.get_ptr(), -1, &textRc,
				DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
			::SetTextColor(dc, oldText);
			::SetBkMode(dc, oldBk);
			if (oldFont != NULL) ::SelectObject(dc, oldFont);

			// Thin outer frame so the loupe reads as one object.
			CRect all = rc;
			::FrameRect(dc, &all, (HBRUSH)::GetStockObject(GRAY_BRUSH));

			// Hint line, only when there is room for it.
			(void)rc;
		}

		// --- state -----------------------------------------------------------
		int m_scale = 1;
		int m_cell = kCellBase;
		int m_side = 0;
		int m_infoH = kInfoBase;
		POINT m_pt = { 0, 0 };
		COLORREF m_color = RGB(0, 0, 0);
		bool m_done = false;
		bool m_accepted = false;
		const char * m_label = nullptr;

		HDC m_screenDC = NULL;   // DISPLAY DC, virtual-desktop coordinate space
		HDC m_sampleDC = NULL;
		HBITMAP m_sampleBmp = NULL;
		HGDIOBJ m_sampleOld = NULL;
	};

} // anonymous namespace

bool spectrum_pick_color_from_screen(HWND owner, const char * label, COLORREF & out) {
	CScreenPickerWnd picker;
	if (!picker.create(owner, label)) return false;

	POINT pt;
	if (!::GetCursorPos(&pt)) pt.x = pt.y = 0;
	picker.begin(pt);

	// Modal loop; ChooseColor and friends work exactly the same way.
	while (!picker.done()) {
		MSG msg;
		const BOOL got = ::GetMessageW(&msg, NULL, 0, 0);
		if (got == 0) { ::PostQuitMessage((int)msg.wParam); break; } // app is shutting down
		if (got == -1) break;
		::TranslateMessage(&msg);
		::DispatchMessageW(&msg);
	}

	picker.force_destroy();

	out = picker.color();
	return picker.accepted();
}
