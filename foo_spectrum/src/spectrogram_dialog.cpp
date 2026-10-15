#include "stdafx.h"
#include "spectrogram.h"
#include "spectrogram_dialog.h"
#include "ui_theme.h"

#include <commdlg.h>
#include <process.h>
#include <SDK/menu.h>
#include <stdio.h>

#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Spectrogram window.
//
// Everything is drawn into one off-screen DIB and presented with a single
// BitBlt, the same way the panel works - the axes and the bitmap are then
// always in step and nothing flickers.
//
// The analysis itself runs on a worker thread, because decoding a whole album
// track takes seconds. Closing the window aborts it and waits.
// ---------------------------------------------------------------------------

namespace {

	enum {
		WM_APP_SPEC_DONE = WM_APP + 101,
		WM_APP_SPEC_PROGRESS = WM_APP + 102,
	};

	enum {
		kClientW = 1300,
		kClientH = 660,
		kPad = 12,
		kBtnH = 26,
		kRowH = 20,
		kGutterL = 62,   // frequency labels
		kGutterB = 30,   // time labels
		kPlotY = 92,
		kCtlH = 22,
	};

	enum {
		id_cur = 4000, id_open, id_axis, id_axismax, id_palette, id_range, id_export, id_close,
		id_line1, id_line2,
	};

	// --- off-screen DIB ----------------------------------------------------

	struct t_dib {
		HDC dc = NULL;
		HBITMAP bmp = NULL;
		HGDIOBJ oldBmp = NULL;
		void * bits = NULL;
		int w = 0, h = 0;

		bool create(HDC ref, int width, int height) {
			destroy();
			if (width <= 0 || height <= 0) return false;

			BITMAPINFO bi = {};
			bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
			bi.bmiHeader.biWidth = width;
			bi.bmiHeader.biHeight = -height;   // top-down
			bi.bmiHeader.biPlanes = 1;
			bi.bmiHeader.biBitCount = 32;
			bi.bmiHeader.biCompression = BI_RGB;

			dc = ::CreateCompatibleDC(ref);
			if (dc == NULL) return false;
			bmp = ::CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
			if (bmp == NULL) { destroy(); return false; }
			oldBmp = ::SelectObject(dc, bmp);
			w = width;
			h = height;
			return true;
		}

		void destroy() {
			if (dc != NULL && oldBmp != NULL) ::SelectObject(dc, oldBmp);
			if (bmp != NULL) ::DeleteObject(bmp);
			if (dc != NULL) ::DeleteDC(dc);
			dc = NULL; bmp = NULL; oldBmp = NULL; bits = NULL; w = h = 0;
		}
	};

	// --- window ------------------------------------------------------------

	class CSpectrogramWindow : public CWindowImpl<CSpectrogramWindow> {
	public:
		DECLARE_WND_CLASS_EX(TEXT("{7C4E1A93-5B2D-4F58-9A31-8D7E2C5B4F63}"),
			CS_HREDRAW | CS_VREDRAW, (HBRUSH)(COLOR_BTNFACE + 1));

		BEGIN_MSG_MAP_EX(CSpectrogramWindow)
			MSG_WM_CREATE(OnCreate)
			MSG_WM_DESTROY(OnDestroy)
			MSG_WM_CLOSE(OnClose)
			MSG_WM_PAINT(OnPaint)
			MSG_WM_ERASEBKGND(OnEraseBkgnd)
			MSG_WM_COMMAND(OnCommand)
			MSG_WM_KEYDOWN(OnKeyDown)
			MESSAGE_HANDLER(WM_APP_SPEC_DONE, OnSpecDone)
			MESSAGE_HANDLER(WM_APP_SPEC_PROGRESS, OnSpecProgress)
		END_MSG_MAP()

		~CSpectrogramWindow() {
			if (m_hWnd != NULL) DestroyWindow();
		}

		bool create(HWND owner) {
			HDC screen = ::GetDC(NULL);
			m_dpi = (screen != NULL) ? ::GetDeviceCaps(screen, LOGPIXELSY) : 96;
			if (screen != NULL) ::ReleaseDC(NULL, screen);
			if (m_dpi <= 0) m_dpi = 96;

			const int w = S(kClientW), h = S(kClientH);
			CRect rc(0, 0, w, h);
			::AdjustWindowRectEx(&rc, WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME, FALSE, WS_EX_TOOLWINDOW);
			if (Create(owner, rc, _T("频谱图 — 判断是否无损"),
					WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_VISIBLE,
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

		static CSpectrogramWindow * g_instance;

	private:
		int S(int px) const { return ::MulDiv(px, m_dpi, 96); }

		HWND mk(const wchar_t * cls, const wchar_t * text, DWORD style,
		        int x, int y, int w, int h, int id) {
			HWND c = ::CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
				S(x), S(y), S(w), S(h), m_hWnd, (HMENU)(INT_PTR)id, NULL, NULL);
			if (c != NULL && m_font.m_hFont != NULL) {
				::SendMessageW(c, WM_SETFONT, (WPARAM)m_font.m_hFont, TRUE);
			}
			return c;
		}

		HWND mk_button(int id, const wchar_t * text, int x, int y, int w) {
			return mk(L"BUTTON", text, BS_PUSHBUTTON | WS_TABSTOP, x, y, w, kBtnH, id);
		}

		HWND mk_combo(int id, int x, int y, int w) {
			return mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
				x, y, w, kCtlH * 8, id);
		}

		HWND mk_label(int id, int x, int y, int w) {
			return mk(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS | SS_CENTERIMAGE,
				x, y, w, kRowH, id);
		}

		// --- lifetime ---------------------------------------------------------

		int OnCreate(LPCREATESTRUCT) {
			NONCLIENTMETRICSW ncm = { sizeof(ncm) };
			if (::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)
				&& m_font.CreateFontIndirectW(&ncm.lfMessageFont) != NULL) {
				m_fontOwned = true;
			} else {
				m_font.m_hFont = (HFONT)::GetStockObject(DEFAULT_GUI_FONT);
			}
			spectrum_ui::apply_rounded_corners(m_hWnd);

			build_controls();
			set_lines(L"还没有分析任何文件。点「分析当前曲目」，或者「打开文件…」。", L"");
			return 0;
		}

		void OnDestroy() {
			stop_analysis();
			m_back.destroy();
			m_image.destroy();
			if (m_fontOwned && m_font.m_hFont != NULL) m_font.DeleteObject();
			m_font.m_hFont = NULL;
			g_instance = nullptr;
			SetMsgHandled(FALSE);
		}

		void OnClose() { DestroyWindow(); }
		void OnFinalMessage(HWND) override { delete this; }

		void OnKeyDown(UINT nChar, UINT, UINT) {
			if (nChar == VK_ESCAPE) DestroyWindow();
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
			const t_spectrogram_view view = spectrogram_view_load();

			int x = kPad;
			mk_button(id_cur, L"分析当前曲目", x, kPad, 110); x += 116;
			mk_button(id_open, L"打开文件…", x, kPad, 100); x += 116;

			mk_label_static_at(L"频率轴", x, kPad - 2, 52); x += 54;
			HWND axis = mk_combo(id_axis, x, kPad, 90);
			::SendMessageW(axis, CB_ADDSTRING, 0, (LPARAM)L"线性");
			::SendMessageW(axis, CB_ADDSTRING, 0, (LPARAM)L"对数");
			::SendMessageW(axis, CB_SETCURSEL, view.logAxis ? 1 : 0, 0);
			x += 100;

			mk_label_static_at(L"调色板", x, kPad - 2, 56); x += 58;
			HWND pal = mk_combo(id_palette, x, kPad, 190);
			for (unsigned i = 0; i < spectrogram_palette_count(); ++i) {
				pfc::stringcvt::string_os_from_utf8 wide(spectrogram_palette_name(i));
				::SendMessageW(pal, CB_ADDSTRING, 0, (LPARAM)wide.get_ptr());
			}
			::SendMessageW(pal, CB_SETCURSEL, view.palette, 0);
			x += 200;

			mk_label_static_at(L"上限", x, kPad - 2, 44); x += 46;
			HWND axmax = mk_combo(id_axismax, x, kPad, 132);
			// Filled in properly once a file has been analysed (the list depends on
			// the source's Nyquist); until then offer everything so it is not empty.
			::SendMessageW(axmax, CB_ADDSTRING, 0, (LPARAM)L"自动");
			{
				wchar_t buf[32];
				for (unsigned i = 0; i < spectrogram_axis_choice_count(); ++i) {
					swprintf_s(buf, _countof(buf), L"%.0f kHz", kSpectrogramAxisChoicesHz[i] / 1000.0);
					::SendMessageW(axmax, CB_ADDSTRING, 0, (LPARAM)buf);
				}
			}
			m_axisMaxHz = view.axisMaxHz;
			::SendMessageW(axmax, CB_SETCURSEL, 0, 0);
			x += 142;

			mk_label_static_at(L"范围", x, kPad - 2, 44); x += 46;
			HWND range = mk_combo(id_range, x, kPad, 150);
			for (unsigned i = 0; i < spectrogram_range_count(); ++i) {
				pfc::stringcvt::string_os_from_utf8 wide(spectrogram_range_name(i));
				::SendMessageW(range, CB_ADDSTRING, 0, (LPARAM)wide.get_ptr());
			}
			::SendMessageW(range, CB_SETCURSEL, view.rangeDb, 0);
			x += 160;

			mk_button(id_export, L"导出 PNG…", x, kPad, 104);
			mk_button(id_close, L"关闭", kClientW - kPad - 76, kPad, 76);

			mk_label(id_line1, kPad, 44, kClientW - 2 * kPad);
			mk_label(id_line2, kPad, 44 + kRowH, kClientW - 2 * kPad);
		}

		void mk_label_static_at(const wchar_t * text, int x, int y, int w) {
			mk(L"STATIC", text, SS_LEFT | SS_CENTERIMAGE, x, y, w, kCtlH, 0);
		}

		void set_lines(const wchar_t * line1, const wchar_t * line2) {
			::SetDlgItemTextW(m_hWnd, id_line1, line1);
			::SetDlgItemTextW(m_hWnd, id_line2, line2);
		}

		void set_status_line2_from_result() {
			if (m_result.width == 0) return;
			pfc::string8 verdict = spectrogram_verdict(m_result);
			pfc::string8 text;
			char buf[320];
			// sprintf_s, not wsprintf: wsprintf cannot do floating point.
			sprintf_s(buf, sizeof(buf), "音乐主体 %.2f kHz ｜ 内容到 %.2f kHz ｜ ",
				m_result.cutoffHz / 1000.0, m_result.cutoffDeepHz / 1000.0);
			text = buf;
			text += verdict.get_ptr();
			pfc::stringcvt::string_os_from_utf8 wide(text.get_ptr());
			::SetDlgItemTextW(m_hWnd, id_line2, wide.get_ptr());
		}

		// --- analysis thread --------------------------------------------------

		void start_analysis(const char * path) {
			if (m_running.load()) return;
			m_pendingPath = path;
			m_abort.reset();
			m_running.store(true);
			m_progress.store(0);

			set_lines(L"正在分析…（解码 + STFT）", L"");
			m_imageDirty = true;
			Invalidate(FALSE);

			m_thread = reinterpret_cast<HANDLE>(
				_beginthreadex(nullptr, 0, &CSpectrogramWindow::thread_proc, this, 0, nullptr));
			if (m_thread == nullptr) {
				m_running.store(false);
				set_lines(L"无法创建分析线程。", L"");
			}
		}

		void stop_analysis() {
			if (m_thread == nullptr) return;
			m_abort.abort();
			::WaitForSingleObject(m_thread, 10000);
			::CloseHandle(m_thread);
			m_thread = nullptr;
			m_running.store(false);
		}

		static unsigned __stdcall thread_proc(void * self) {
			static_cast<CSpectrogramWindow *>(self)->run_analysis();
			return 0;
		}

		void run_analysis() {
			t_spectrogram_result result;
			pfc::string8 error;

			t_spectrogram_params params;
			// Finer than any window this will be shown in. The renderer resamples
			// from here, and a grid that is finer than the display is what keeps
			// the picture smooth instead of blocky.
			params.width = 3000;
			params.height = 1024;
			params.fftSize = 4096;

			const bool ok = spectrogram_analyse(
				m_pendingPath.c_str(), params, result, m_abort,
				[this](double frac) {
					if (m_hWnd != NULL) {
						::PostMessageW(m_hWnd, WM_APP_SPEC_PROGRESS,
							(WPARAM)(int)(frac * 100.0), 0);
					}
				},
				error);

			{
				std::lock_guard<std::mutex> lock(m_resultMutex);
				if (ok) {
					m_result = std::move(result);
				} else {
					m_result = t_spectrogram_result();
					m_error = error;
				}
			}
			m_ok = ok;
			if (m_hWnd != NULL) ::PostMessageW(m_hWnd, WM_APP_SPEC_DONE, 0, 0);
		}

		LRESULT OnSpecProgress(UINT, WPARAM wParam, LPARAM, BOOL & handled) {
			handled = TRUE;
			if (!m_running.load()) return 0;
			// The status line is a static control, so it can be updated from here.
			wchar_t buf[64];
			wsprintfW(buf, L"正在分析… %d%%（解码 + STFT）", (int)wParam);
			::SetDlgItemTextW(m_hWnd, id_line1, buf);
			return 0;
		}

		LRESULT OnSpecDone(UINT, WPARAM, LPARAM, BOOL & handled) {
			handled = TRUE;
			m_running.store(false);
			if (m_thread != nullptr) {
				::CloseHandle(m_thread);
				m_thread = nullptr;
			}

			if (m_ok) {
				t_spectrogram_result result;
				{
					std::lock_guard<std::mutex> lock(m_resultMutex);
					result = m_result;
				}
				m_result = std::move(result);
				m_imageDirty = true;

				// The ceiling list depends on this file's Nyquist limit, so it is
				// rebuilt here rather than at creation time.
				rebuild_axis_max_combo();

				pfc::string8 line1;
				char buf[512];
				sprintf_s(buf, sizeof(buf), "%s    %.1f 秒   %.1f kHz 采样   %u 声道",
					m_pendingPath.c_str(), m_result.durationSec,
					m_result.sampleRate / 1000.0, m_result.channels);
				line1 = buf;
				pfc::stringcvt::string_os_from_utf8 wide(line1.get_ptr());
				::SetDlgItemTextW(m_hWnd, id_line1, wide.get_ptr());
				set_status_line2_from_result();
			} else {
				pfc::string8 line;
				line = "分析失败：";
				line += m_error.get_ptr();
				pfc::stringcvt::string_os_from_utf8 wide(line.get_ptr());
				::SetDlgItemTextW(m_hWnd, id_line1, wide.get_ptr());
				::SetDlgItemTextW(m_hWnd, id_line2, L"");
				m_imageDirty = true;
			}
			Invalidate(FALSE);
			return 0;
		}

		// --- commands ---------------------------------------------------------

		void OnCommand(UINT code, int id, CWindow) {
			if (id == id_close) { DestroyWindow(); return; }
			if (id == id_cur) { analyse_current(); return; }
			if (id == id_open) { analyse_picked(); return; }
			if (id == id_export) { export_png(); return; }
			if (id == id_axismax && code == CBN_SELCHANGE) {
				axis_max_from_combo();
				m_imageDirty = true;
				save_view();
				Invalidate(FALSE);
				return;
			}
			if ((id == id_axis || id == id_palette || id == id_range) && code == CBN_SELCHANGE) {
				m_imageDirty = true;
				save_view();
				Invalidate(FALSE);
				return;
			}
		}

		int combo_sel(int id) const {
			const int v = (int)::SendDlgItemMessageW(m_hWnd, id, CB_GETCURSEL, 0, 0);
			return (v < 0) ? 0 : v;
		}

		//! Upper end of the frequency axis, in Hz, never above the source's own
		//! Nyquist - there is nothing up there to draw.
		double axis_max_effective() const {
			if (m_result.maxFreqHz <= 0.0) return 22050.0;
			if (m_axisMaxHz <= 0.0 || m_axisMaxHz > m_result.maxFreqHz) return m_result.maxFreqHz;
			return m_axisMaxHz;
		}

		//! Rebuilds the ceiling list for the file that was just analysed.
		//!
		//! Only entries that actually crop are offered. Listing a ceiling above
		//! the source's Nyquist would hand the user an option that cannot change
		//! anything, which reads as a broken control.
		void rebuild_axis_max_combo() {
			HWND c = ::GetDlgItem(m_hWnd, id_axismax);
			if (c == NULL) return;
			::SendMessageW(c, CB_RESETCONTENT, 0, 0);

			const double nyquist = m_result.maxFreqHz;
			wchar_t buf[64];
			::SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"自动");

			for (unsigned i = 0; i < spectrogram_axis_choice_count(); ++i) {
				const double hz = kSpectrogramAxisChoicesHz[i];
				if (hz >= nyquist) continue;   // could not crop anything
				swprintf_s(buf, _countof(buf), L"%.0f kHz", hz / 1000.0);
				::SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)buf);
			}

			if (m_axisMaxHz > 0.0 && m_axisMaxHz < nyquist) {
				wchar_t want[64];
				swprintf_s(want, _countof(want), L"%.0f kHz", m_axisMaxHz / 1000.0);
				const LRESULT idx = ::SendMessageW(c, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)want);
				if (idx > 0) {
					::SendMessageW(c, CB_SETCURSEL, (WPARAM)idx, 0);
					return;
				}
			}
			m_axisMaxHz = 0.0;
			::SendMessageW(c, CB_SETCURSEL, 0, 0);
		}

		//! Maps the combo's position back to a ceiling. Index 0 is auto and the
		//! rest follow the filtered order built above.
		void axis_max_from_combo() {
			const int sel = combo_sel(id_axismax);
			m_axisMaxHz = 0.0;
			if (sel <= 0) return;
			const double nyquist = (m_result.maxFreqHz > 0.0) ? m_result.maxFreqHz : 1e12;
			unsigned seen = 0;
			for (unsigned i = 0; i < spectrogram_axis_choice_count(); ++i) {
				const double hz = kSpectrogramAxisChoicesHz[i];
				if (hz >= nyquist) continue;
				if (++seen == (unsigned)sel) { m_axisMaxHz = hz; return; }
			}
		}

		void save_view() const {
			t_spectrogram_view v;
			v.logAxis = (combo_sel(id_axis) == 1) ? 1 : 0;
			v.palette = (unsigned)combo_sel(id_palette);
			v.axisMaxHz = m_axisMaxHz;
			v.rangeDb = (unsigned)combo_sel(id_range);
			spectrogram_view_save(v);
		}

		void analyse_current() {
			if (m_running.load()) return;
			metadb_handle_ptr track;
			auto pc = playback_control::get();
			if (!pc.is_valid() || !pc->get_now_playing(track) || !track.is_valid()) {
				set_lines(L"当前没有正在播放的曲目。", L"");
				return;
			}
			const char * path = track->get_path();
			if (path == NULL || *path == 0) {
				set_lines(L"当前曲目没有可分析的文件路径。", L"");
				return;
			}
			start_analysis(path);
		}

		void analyse_picked() {
			if (m_running.load()) return;

			wchar_t path[MAX_PATH] = L"";
			OPENFILENAMEW ofn = { sizeof(ofn) };
			ofn.hwndOwner = m_hWnd;
			ofn.lpstrFilter =
				L"音频文件 (*.flac;*.wav;*.ape;*.wv;*.m4a;*.mp3;*.ogg;*.opus;*.aiff)\0"
				L"*.flac;*.wav;*.ape;*.wv;*.m4a;*.mp3;*.ogg;*.opus;*.aiff\0"
				L"所有文件 (*.*)\0*.*\0\0";
			ofn.lpstrFile = path;
			ofn.nMaxFile = MAX_PATH;
			ofn.lpstrTitle = L"选择要分析的音频文件";
			ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
			if (!::GetOpenFileNameW(&ofn)) return;

			pfc::stringcvt::string_utf8_from_os utf8(path);
			start_analysis(utf8.get_ptr());
		}

		// --- PNG export -------------------------------------------------------

		static int find_encoder(const wchar_t * mime, CLSID & out) {
			UINT num = 0, size = 0;
			Gdiplus::GetImageEncodersSize(&num, &size);
			if (size == 0) return -1;
			std::vector<BYTE> buf(size);
			Gdiplus::ImageCodecInfo * info = (Gdiplus::ImageCodecInfo *)buf.data();
			Gdiplus::GetImageEncoders(num, size, info);
			for (UINT i = 0; i < num; ++i) {
				if (wcscmp(info[i].MimeType, mime) == 0) { out = info[i].Clsid; return (int)i; }
			}
			return -1;
		}

		void export_png() {
			if (m_result.width == 0) {
				set_lines(L"还没有可导出的图。先分析一个文件。", L"");
				return;
			}

			wchar_t path[MAX_PATH] = L"spectrogram.png";
			OPENFILENAMEW ofn = { sizeof(ofn) };
			ofn.hwndOwner = m_hWnd;
			ofn.lpstrFilter = L"PNG 图片 (*.png)\0*.png\0所有文件 (*.*)\0*.*\0\0";
			ofn.lpstrFile = path;
			ofn.nMaxFile = MAX_PATH;
			ofn.lpstrDefExt = L"png";
			ofn.lpstrTitle = L"导出频谱图";
			ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;
			if (!::GetSaveFileNameW(&ofn)) return;

			// Export at the analysis resolution: a fixed, generous size that does
			// not depend on how the window happens to be sized.
			HDC ref = (m_back.dc != NULL) ? m_back.dc : ::GetDC(NULL);
			t_dib out;
			if (!render_into(out, ref, (int)m_result.width, (int)m_result.height)) {
				if (m_back.dc == NULL && ref != NULL) ::ReleaseDC(NULL, ref);
				::MessageBoxW(m_hWnd, L"无法生成导出图像。", L"导出失败", MB_ICONERROR | MB_OK);
				return;
			}
			if (m_back.dc == NULL && ref != NULL) ::ReleaseDC(NULL, ref);

			Gdiplus::GdiplusStartupInput input;
			ULONG_PTR token = 0;
			if (Gdiplus::GdiplusStartup(&token, &input, NULL) != Gdiplus::Ok) {
				out.destroy();
				::MessageBoxW(m_hWnd, L"无法初始化 GDI+。", L"导出失败", MB_ICONERROR | MB_OK);
				return;
			}

			bool ok = false;
			CLSID pngClsid = {};
			if (find_encoder(L"image/png", pngClsid) >= 0) {
				Gdiplus::Bitmap * bmp = Gdiplus::Bitmap::FromHBITMAP(out.bmp, NULL);
				if (bmp != NULL) {
					ok = (bmp->Save(path, &pngClsid, NULL) == Gdiplus::Ok);
					delete bmp;
				}
			}
			Gdiplus::GdiplusShutdown(token);
			out.destroy();

			if (!ok) {
				::MessageBoxW(m_hWnd, L"写入 PNG 失败。", L"导出失败", MB_ICONERROR | MB_OK);
			}
		}

		// --- painting ---------------------------------------------------------

		BOOL OnEraseBkgnd(CDCHandle dc) {
			CRect rc;
			GetClientRect(&rc);
			dc.FillRect(&rc, (HBRUSH)(COLOR_BTNFACE + 1));
			return TRUE;
		}

		void plot_rect(CRect & rc) const {
			CRect client;
			GetClientRect(&client);
			rc = CRect(S(kPad) + S(kGutterL), S(kPlotY),
				client.right - S(kPad), client.bottom - S(kGutterB));
			if (rc.right <= rc.left) rc.right = rc.left + 1;
			if (rc.bottom <= rc.top) rc.bottom = rc.top + 1;
		}

		//! Renders the cell grid into a DIB of exactly the requested pixel size.
		//!
		//! The cells are resampled here rather than left to StretchBlt. Nearest
		//! neighbour scaling of a 3000-wide image down to a ~1200 px window drops
		//! columns irregularly and leaves a visibly blocky, mosaic-like texture.
		//! Averaging the source columns a pixel covers, and interpolating between
		//! the two neighbouring rows, gives the smooth wash a spectrogram should
		//! look like; the result is then blitted 1:1.
		bool render_into(t_dib & dst, HDC ref, int w, int h) {
			if (m_result.width == 0 || m_result.height == 0 || w <= 0 || h <= 0) return false;
			if (!dst.create(ref, w, h)) return false;

			const bool logAxis = (combo_sel(id_axis) == 1);
			const unsigned palette = (unsigned)combo_sel(id_palette);
			const double rangeDb = spectrogram_range_value((unsigned)combo_sel(id_range));
			const double nyquist = m_result.maxFreqHz;
			const double axisMax = axis_max_effective();
			const unsigned SW = m_result.width, SH = m_result.height;
			const unsigned char * cells = m_result.cells.data();
			uint32_t * px = (uint32_t *)dst.bits;
			const uint32_t floorColor = spectrogram_palette_color(palette, 0);

			for (int y = 0; y < h; ++y) {
				uint32_t * outRow = px + (size_t)y * w;

				// Frequency at the centre of this output row.
				const double fromTop = (h > 1) ? ((double)y + 0.5) / (double)h : 0.0;
				const double freq = logAxis
					? 20.0 * pow(axisMax / 20.0, 1.0 - fromTop)
					: axisMax * (1.0 - fromTop);

				// Above the source's own Nyquist there is nothing to show.
				if (freq > nyquist || SH < 2) {
					for (int x = 0; x < w; ++x) outRow[x] = floorColor;
					continue;
				}

				const double sf = freq / nyquist * (double)(SH - 1);
				unsigned r0 = (unsigned)sf;
				if (r0 > SH - 2) r0 = SH - 2;
				const unsigned r1 = r0 + 1;
				const double fr = sf - (double)r0;
				const unsigned char * rowA = cells + (size_t)r0 * SW;
				const unsigned char * rowB = cells + (size_t)r1 * SW;

				for (int x = 0; x < w; ++x) {
					unsigned c0 = (unsigned)((double)x * (double)SW / (double)w);
					unsigned c1 = (unsigned)(((double)x + 1.0) * (double)SW / (double)w);
					if (c1 <= c0) c1 = c0 + 1;
					if (c0 >= SW) c0 = SW - 1;
					if (c1 > SW) c1 = SW;

					double acc = 0.0;
					for (unsigned c = c0; c < c1; ++c) {
						acc += (double)rowA[c] * (1.0 - fr) + (double)rowB[c] * fr;
					}
					acc /= (double)(c1 - c0);

					const double db = acc / 255.0 * kCellStoreRangeDb - kCellStoreRangeDb;
					double v = (db + rangeDb) / rangeDb * 255.0;
					if (!(v > 0.0)) v = 0.0;      // also catches NaN
					if (v > 255.0) v = 255.0;
					outRow[x] = spectrogram_palette_color(palette, (unsigned)v);
				}
			}
			return true;
		}

		void OnPaint(CDCHandle) {
			CPaintDC dcPaint(*this);
			CRect client;
			if (!GetClientRect(&client)) return;
			if (client.Width() <= 0 || client.Height() <= 0) return;

			if (m_back.w == client.Width() && m_back.h == client.Height()) {
				// reuse
			} else if (!m_back.create(dcPaint, client.Width(), client.Height())) {
				return;
			}

			HDC dc = m_back.dc;
			::FillRect(dc, &client, (HBRUSH)(COLOR_BTNFACE + 1));

			CRect plot;
			plot_rect(plot);

			// Re-render when a control changed, or when the plot was resized - the
			// image is produced at exactly the plot size, so the blit below is 1:1
			// and nothing is left to StretchBlt's nearest-neighbour scaling.
			if (m_result.width == 0) {
				m_image.destroy();
			} else if (m_imageDirty || m_image.w != plot.Width() || m_image.h != plot.Height()) {
				if (render_into(m_image, dc, plot.Width(), plot.Height())) m_imageDirty = false;
			}

			if (m_image.bmp != NULL) {
				::BitBlt(dc, plot.left, plot.top, plot.Width(), plot.Height(),
					m_image.dc, 0, 0, SRCCOPY);
			} else {
				CRect empty = plot;
				HBRUSH black = ::CreateSolidBrush(RGB(0, 0, 0));
				::FillRect(dc, &empty, black);
				::DeleteObject(black);
				const wchar_t * hint = m_running.load()
					? L"正在分析…"
					: L"点「分析当前曲目」或「打开文件…」开始";
				::SetBkMode(dc, TRANSPARENT);
				::SetTextColor(dc, RGB(0xC0, 0xC8, 0xD0));
				::DrawTextW(dc, hint, -1, &empty, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
			}

			draw_axes(dc, plot);

			::BitBlt(dcPaint, 0, 0, client.Width(), client.Height(), dc, 0, 0, SRCCOPY);
		}

		void draw_axes(HDC dc, const CRect & plot) {
			if (m_result.width == 0) return;

			HFONT oldFont = NULL;
			if (m_font.m_hFont != NULL) oldFont = (HFONT)::SelectObject(dc, m_font.m_hFont);
			const int oldBk = ::SetBkMode(dc, TRANSPARENT);

			const COLORREF axisCol = ::GetSysColor(COLOR_GRAYTEXT);
			const COLORREF textCol = ::GetSysColor(COLOR_BTNTEXT);
			HPEN pen = ::CreatePen(PS_SOLID, 1, axisCol);
			HGDIOBJ oldPen = ::SelectObject(dc, pen);

			const bool logAxis = (combo_sel(id_axis) == 1);
			const double axisMax = axis_max_effective();

			// Tick spacing. A log axis wants the usual landmark frequencies; a
			// linear axis wants a round step, and the step has to grow once the
			// ceiling runs up to 96 kHz or the gutter turns into a smear.
			std::vector<double> ticks;
			if (logAxis) {
				const double cand[] = { 20, 50, 100, 200, 500, 1000, 2000, 5000,
					10000, 20000, 50000, 100000 };
				for (double c : cand) { if (c <= axisMax) ticks.push_back(c); }
			} else {
				double step = 2000.0;
				if (axisMax > 24000.0)  step = 5000.0;
				if (axisMax > 50000.0)  step = 10000.0;
				if (axisMax > 100000.0) step = 20000.0;
				for (double hz = 0.0; hz <= axisMax + 0.5; hz += step) ticks.push_back(hz);
			}

			const double logSpan = (axisMax > 20.0) ? log(axisMax / 20.0) : 1.0;

			// Frequency axis: labels in the left gutter, ticks touching the plot.
			for (double hz : ticks) {
				const double t = logAxis
					? ((hz <= 20.0) ? 0.0 : log(hz / 20.0) / logSpan)
					: hz / axisMax;
				int y = plot.bottom - (int)(t * (double)plot.Height() + 0.5);
				if (y < plot.top) y = plot.top;
				if (y > plot.bottom) continue;

				::MoveToEx(dc, plot.left - 4, y, NULL);
				::LineTo(dc, plot.left, y);

				wchar_t label[16];
				if (hz >= 1000.0) swprintf_s(label, _countof(label), L"%.0fk", hz / 1000.0);
				else              swprintf_s(label, _countof(label), L"%.0f", hz);
				RECT r = { plot.left - S(kGutterL), y - 8, plot.left - 6, y + 8 };
				::SetTextColor(dc, textCol);
				::DrawTextW(dc, label, -1, &r, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
			}

			// Time axis.
			const double dur = m_result.durationSec;
			if (dur > 0.0) {
				double step = 10.0;
				if (dur <= 30.0) step = 5.0;
				else if (dur <= 120.0) step = 15.0;
				else if (dur <= 300.0) step = 30.0;
				else if (dur <= 900.0) step = 60.0;
				else step = 120.0;

				for (double t = 0.0; t <= dur + 0.001; t += step) {
					const int x = plot.left + (int)(t / dur * (double)plot.Width() + 0.5);
					if (x > plot.right) break;
					::MoveToEx(dc, x, plot.bottom, NULL);
					::LineTo(dc, x, plot.bottom + 4);

					const int total = (int)(t + 0.5);
					wchar_t label[24];
					wsprintfW(label, L"%d:%02d", total / 60, total % 60);
					RECT r = { x - 30, plot.bottom + 5, x + 30, plot.bottom + 5 + kRowH };
					::SetTextColor(dc, textCol);
					::DrawTextW(dc, label, -1, &r, DT_CENTER | DT_TOP | DT_SINGLELINE);
				}
			}

			// Cutoff marker: the whole reason this window exists.
			// Two markers, because they answer different questions. The music band
			// edge is what the eye reads off the picture; the deep edge is where
			// the file's content actually stops, and the gap between them is what
			// separates a lossy brick wall from an ordinary rolloff.
			auto mark_freq = [&](double hz, COLORREF col, const wchar_t * text, bool labelAbove) {
				const double t = logAxis
					? ((hz <= 20.0) ? 0.0 : log(hz / 20.0) / logSpan)
					: hz / axisMax;
				int y = plot.bottom - (int)(t * (double)plot.Height() + 0.5);
				if (y < plot.top) y = plot.top;
				if (y > plot.bottom) y = plot.bottom;

				HPEN mark = ::CreatePen(PS_DOT, 1, col);
				HGDIOBJ prev = ::SelectObject(dc, mark);
				::MoveToEx(dc, plot.left, y, NULL);
				::LineTo(dc, plot.right, y);
				::SelectObject(dc, prev);
				::DeleteObject(mark);

				SIZE sz = {};
				::GetTextExtentPoint32W(dc, text, (int)wcslen(text), &sz);
				RECT r;
				if (labelAbove) {
					r = { plot.left + 6, y - sz.cy - 4, plot.left + 6 + sz.cx + 8, y - 4 };
					if (r.top < plot.top) { r.top = y + 4; r.bottom = y + 4 + sz.cy; }
				} else {
					r = { plot.left + 6, y + 3, plot.left + 6 + sz.cx + 8, y + 3 + sz.cy };
				}
				::SetTextColor(dc, col);
				::DrawTextW(dc, text, -1, &r, DT_LEFT | DT_TOP | DT_SINGLELINE);
			};

			// Which markers are worth drawing depends on what the numbers mean.
			// When content runs all the way to the sampling ceiling there is no
			// "edge" to point at up there - drawing one would just label the top
			// of the data. When the deep edge sits on top of the music edge there
			// is a single brick wall, and one line says it.
			const double deep = m_result.cutoffDeepHz;
			const double main = m_result.cutoffHz;
			const bool cleanCliff = (deep > 0.0) && (main > 0.0) && (deep <= main * 1.05);
			const bool deepIsCeiling = (deep >= m_result.maxFreqHz * 0.95);
			const bool showDeep = (deep > 0.0) && !deepIsCeiling && !cleanCliff
				&& (deep > main * 1.05);

			if (showDeep) {
				wchar_t label[64];
				swprintf_s(label, _countof(label), L"内容到 %.1f kHz", deep / 1000.0);
				mark_freq(deep, RGB(0xB0, 0x30, 0x30), label, true);
			}
			if (main > 0.0) {
				wchar_t label[64];
				if (cleanCliff) {
					swprintf_s(label, _countof(label), L"截止 %.1f kHz", main / 1000.0);
				} else {
					swprintf_s(label, _countof(label), L"音乐主体 %.1f kHz", main / 1000.0);
				}
				mark_freq(main, RGB(0xFF, 0x40, 0x40), label, !showDeep);
			}

			::SelectObject(dc, oldPen);
			::DeleteObject(pen);
			::SetBkMode(dc, oldBk);
			if (oldFont != NULL) ::SelectObject(dc, oldFont);
		}

	private:
		int m_dpi = 96;
		CFont m_font;
		bool m_fontOwned = false;
		t_dib m_back;
		t_dib m_image;
		bool m_imageDirty = true;

		t_spectrogram_result m_result;
		std::mutex m_resultMutex;
		pfc::string8 m_error;
		bool m_ok = false;
		//! Chosen upper end of the frequency axis in Hz; 0 = follow the source.
		double m_axisMaxHz = 0.0;

		std::string m_pendingPath;
		abort_callback_impl m_abort;
		std::atomic<bool> m_running{ false };
		std::atomic<int> m_progress{ 0 };
		HANDLE m_thread = nullptr;
	};

	CSpectrogramWindow * CSpectrogramWindow::g_instance = nullptr;

} // 匿名命名空间

void spectrum_open_spectrogram_window(HWND owner) {
	if (CSpectrogramWindow::g_instance != nullptr) {
		CSpectrogramWindow::g_instance->bring_to_front();
		return;
	}
	CSpectrogramWindow * w = new CSpectrogramWindow();
	if (!w->create(owner)) {
		delete w;
		return;
	}
	CSpectrogramWindow::g_instance = w;
}

// ---------------------------------------------------------------------------
// Main menu entry.
//
// The right-click entry on the spectrum panel only exists while that panel is
// on screen, which in a custom UI layout can be awkward to arrange. Hanging a
// command off the standard visualisations group puts it in 视图 → 可视化 and
// makes it reachable regardless of the layout.
// ---------------------------------------------------------------------------

namespace {

	static const GUID guid_cmd_spectrogram =
		{ 0xa1b2c3d4, 0x0010, 0x4a01, { 0x90, 0x10, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 } };

	class mainmenu_spectrogram : public mainmenu_commands {
	public:
		t_uint32 get_command_count() override { return 1; }

		GUID get_command(t_uint32) override { return guid_cmd_spectrogram; }

		void get_name(t_uint32, pfc::string_base & out) override {
			out = "频谱图（判断是否无损）…";
		}

		bool get_description(t_uint32, pfc::string_base & out) override {
			out = "整首曲子画成时间×频率热力图，并给出估计的截止频率。";
			return true;
		}

		GUID get_parent() override { return mainmenu_groups::view_visualisations; }

		void execute(t_uint32, ctx_t) override {
			spectrum_open_spectrogram_window(nullptr);
		}
	};

	static mainmenu_commands_factory_t<mainmenu_spectrogram> g_mainmenu_spectrogram;

} // 匿名命名空间
