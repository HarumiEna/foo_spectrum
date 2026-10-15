#include "stdafx.h"
#include "spectrogram.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <stdio.h>
#include <vector>

// ---------------------------------------------------------------------------
// Implementation notes
//
// The panel borrows foobar2000's FFT (visualisation_manager) and therefore only
// ever sees the audio around the playback cursor. A whole-file spectrogram needs
// its own transform, so this file carries a small radix-2 FFT plus the decode
// loop that feeds it.
//
// Work is streamed: a ring buffer holds the last fftSize mono samples, a frame
// is transformed every hop samples, and each frame is folded straight into the
// output column for its timestamp. That keeps memory at one byte per output cell
// no matter how long the file is.
// ---------------------------------------------------------------------------

namespace {

	const double kPi = 3.14159265358979323846;

	// --- radix-2 FFT -------------------------------------------------------

	class CFft {
	public:
		bool init(unsigned n) {
			if (n < 4 || (n & (n - 1)) != 0) return false;
			m_n = n;
			m_levels = 0;
			while ((1u << m_levels) < n) ++m_levels;

			m_rev.resize(n);
			for (unsigned i = 0; i < n; ++i) {
				unsigned r = 0;
				for (unsigned b = 0; b < m_levels; ++b) {
					if (i & (1u << b)) r |= 1u << (m_levels - 1 - b);
				}
				m_rev[i] = r;
			}

			m_cos.resize(n / 2);
			m_sin.resize(n / 2);
			for (unsigned i = 0; i < n / 2; ++i) {
				const double a = -2.0 * kPi * (double)i / (double)n;
				m_cos[i] = (float)cos(a);
				m_sin[i] = (float)sin(a);
			}
			return true;
		}

		unsigned size() const { return m_n; }

		//! In-place forward transform of m_n complex points.
		void forward(float * re, float * im) const {
			const unsigned n = m_n;

			for (unsigned i = 0; i < n; ++i) {
				const unsigned j = m_rev[i];
				if (j > i) {
					std::swap(re[i], re[j]);
					std::swap(im[i], im[j]);
				}
			}

			for (unsigned len = 2; len <= n; len <<= 1) {
				const unsigned half = len >> 1;
				const unsigned step = n / len;
				for (unsigned base = 0; base < n; base += len) {
					for (unsigned k = 0; k < half; ++k) {
						const unsigned tw = k * step;
						const float wr = m_cos[tw], wi = m_sin[tw];
						const unsigned a = base + k, b = a + half;
						const float xr = re[b] * wr - im[b] * wi;
						const float xi = re[b] * wi + im[b] * wr;
						re[b] = re[a] - xr;
						im[b] = im[a] - xi;
						re[a] += xr;
						im[a] += xi;
					}
				}
			}
		}

	private:
		unsigned m_n = 0, m_levels = 0;
		std::vector<unsigned> m_rev;
		std::vector<float> m_cos, m_sin;
	};

	// --- palettes ----------------------------------------------------------

	struct t_palette_stop {
		unsigned char pos;   // 0..255
		unsigned char r, g, b;
	};

	// Black -> blue -> cyan -> green -> yellow -> orange -> red.
	//
	// The top stop is deliberately a saturated red, not white. Real material -
	// especially anything with ultrasonic content - puts plenty of cells near the
	// top of the scale, and a white top end turns those into featureless white
	// blobs that hide the very structure the picture exists to show.
	const t_palette_stop kClassic[] = {
		{   0, 0x00, 0x00, 0x00 }, {  32, 0x00, 0x00, 0x60 }, {  64, 0x00, 0x20, 0xC0 },
		{  96, 0x00, 0x90, 0xE0 }, { 128, 0x00, 0xC0, 0x80 }, { 160, 0x60, 0xD0, 0x00 },
		{ 192, 0xE0, 0xE0, 0x00 }, { 224, 0xFF, 0xA0, 0x00 }, { 255, 0xFF, 0x30, 0x00 },
	};
	// Spek's own colour ramp: black -> deep purple -> magenta -> red -> orange ->
	// yellow -> white. This is the look people coming from Spek expect to see, so
	// it is offered alongside the others rather than replacing them.
	const t_palette_stop kHeat[] = {
		{   0, 0x00, 0x00, 0x00 }, {  32, 0x1A, 0x0A, 0x40 }, {  64, 0x4B, 0x0F, 0x8C },
		{  96, 0x8B, 0x1A, 0xA8 }, { 128, 0xC4, 0x2B, 0x8A }, { 160, 0xE8, 0x45, 0x50 },
		{ 192, 0xF7, 0x7A, 0x1E }, { 224, 0xFD, 0xC0, 0x2E }, { 255, 0xFF, 0xFF, 0xFF },
	};
	const t_palette_stop kGrayscale[] = {
		{   0, 0x00, 0x00, 0x00 }, { 255, 0xFF, 0xFF, 0xFF },
	};
	const t_palette_stop kAqua[] = {
		{   0, 0x00, 0x00, 0x00 }, {  64, 0x10, 0x10, 0x60 }, { 128, 0x20, 0x60, 0xC0 },
		{ 192, 0x40, 0xC0, 0xE0 }, { 255, 0x30, 0xE0, 0xFF },
	};

	struct t_palette {
		const char * name;
		const t_palette_stop * stops;
		unsigned count;
	};

	const t_palette kPalettes[] = {
		{ "经典",   kClassic,   sizeof(kClassic)   / sizeof(kClassic[0]) },
		{ "热力图", kHeat,      sizeof(kHeat)      / sizeof(kHeat[0]) },
		{ "灰度",   kGrayscale, sizeof(kGrayscale) / sizeof(kGrayscale[0]) },
		{ "青蓝",   kAqua,      sizeof(kAqua)      / sizeof(kAqua[0]) },
	};

	// --- dynamic range -----------------------------------------------------

	struct t_range_opt {
		const char * name;
		double db;
	};

	// A narrow range gives punchy contrast but crushes quiet detail to black; a
	// wide one is what lets the top octave fade out gradually instead of ending
	// in a hard edge. 120 dB is Spek's own scale, so that is the default.
	const t_range_opt kRangeDb[] = {
		{ "70 dB",   70.0 },
		{ "90 dB",   90.0 },
		{ "110 dB", 110.0 },
		{ "120 dB", 120.0 },
		{ "140 dB", 140.0 },
		{ "160 dB", 160.0 },
	};

	const unsigned kDefaultRangeIndex = 3;   // 120 dB

	// --- axis maxima -------------------------------------------------------
	//
	// The choosable ceilings live in kSpectrogramAxisChoicesHz, defined below the
	// anonymous namespace because the header exposes it. The window lists only
	// the entries at or below the current source's Nyquist limit.

	//! View options live in the component's own configuration, like the colour
	//! presets - they are a UI preference, not something to expose as an
	//! Advanced Preferences entry.
	constexpr GUID guid_specView = { 0xa1b2c3d4, 0x0011, 0x4a01, { 0x90, 0x11, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 } };
	cfg_string g_specView(guid_specView, "");

} // 匿名命名空间

const double kSpectrogramAxisChoicesHz[] = { 20000.0, 40000.0, 48000.0, 80000.0, 96000.0 };

unsigned spectrogram_axis_choice_count() {
	return (unsigned)(sizeof(kSpectrogramAxisChoicesHz) / sizeof(kSpectrogramAxisChoicesHz[0]));
}

unsigned spectrogram_range_count() {
	return (unsigned)(sizeof(kRangeDb) / sizeof(kRangeDb[0]));
}

const char * spectrogram_range_name(unsigned index) {
	if (index >= spectrogram_range_count()) return "";
	return kRangeDb[index].name;
}

double spectrogram_range_value(unsigned index) {
	if (index >= spectrogram_range_count()) return 120.0;
	return kRangeDb[index].db;
}

t_spectrogram_view spectrogram_view_load() {
	t_spectrogram_view v;
	v.rangeDb = kDefaultRangeIndex;
	const char * p = g_specView.get();
	if (p == nullptr || *p == 0) return v;

	// "axis|palette|maxHz|range"
	unsigned field[4] = { 0, 0, 0, 0 };
	unsigned n = 0;
	while (n < 4 && *p != 0) {
		if (n == 2) {
			// Field 2 is a frequency and can be large; parse it as its own type
			// rather than truncating it into the small integer fields.
			double value = 0.0;
			while (*p >= '0' && *p <= '9') { value = value * 10.0 + (*p - '0'); ++p; }
			v.axisMaxHz = value;
		} else {
			int value = 0;
			bool neg = false;
			if (*p == '-') { neg = true; ++p; }
			while (*p >= '0' && *p <= '9') { value = value * 10 + (*p - '0'); ++p; }
			field[n] = (unsigned)(neg ? -value : value);
		}
		++n;
		if (*p == '|') ++p; else break;
	}
	v.logAxis = field[0] ? 1 : 0;
	v.palette = field[1];
	if (n >= 4) v.rangeDb = field[3];

	if (v.palette >= spectrogram_palette_count()) v.palette = 0;
	if (v.rangeDb >= spectrogram_range_count()) v.rangeDb = kDefaultRangeIndex;
	// Older builds stored an index into the ceiling list here (0..5). Those
	// values are not frequencies, so treat anything implausibly low as "auto"
	// rather than zooming the axis down to a few hertz.
	if (v.axisMaxHz > 0.0 && v.axisMaxHz < 1000.0) v.axisMaxHz = 0.0;
	return v;
}

void spectrogram_view_save(const t_spectrogram_view & v) {
	char buf[64];
	// The ceiling is written as whole Hz; a fractional axis limit is meaningless.
	sprintf_s(buf, sizeof(buf), "%u|%u|%.0f|%u",
		v.logAxis ? 1 : 0, v.palette, v.axisMaxHz, v.rangeDb);
	g_specView.set(buf);
}

unsigned spectrogram_palette_count() {
	return (unsigned)(sizeof(kPalettes) / sizeof(kPalettes[0]));
}

const char * spectrogram_palette_name(unsigned palette) {
	if (palette >= spectrogram_palette_count()) return "";
	return kPalettes[palette].name;
}

unsigned spectrogram_palette_color(unsigned palette, unsigned intensity) {
	if (palette >= spectrogram_palette_count()) palette = 0;
	if (intensity > 255) intensity = 255;

	const t_palette & p = kPalettes[palette];
	const unsigned char v = (unsigned char)intensity;

	unsigned i = 0;
	while (i + 2 < p.count && p.stops[i + 1].pos <= v) ++i;

	const t_palette_stop & a = p.stops[i];
	const t_palette_stop & b = p.stops[(i + 1 < p.count) ? i + 1 : i];
	if (b.pos == a.pos) return ((unsigned)a.r << 16) | ((unsigned)a.g << 8) | a.b;

	const unsigned span = (unsigned)b.pos - (unsigned)a.pos;
	const unsigned t = ((unsigned)v - (unsigned)a.pos) * 255u / span;
	const unsigned r = (unsigned)a.r + (((int)b.r - (int)a.r) * (int)t) / 255;
	const unsigned g = (unsigned)a.g + (((int)b.g - (int)a.g) * (int)t) / 255;
	const unsigned bl = (unsigned)a.b + (((int)b.b - (int)a.b) * (int)t) / 255;
	return (r << 16) | (g << 8) | bl;
}

pfc::string8 spectrogram_verdict(const t_spectrogram_result & r) {
	pfc::string8 out;
	// NOTE: wsprintf does not support floating point - it would garble the whole
	// argument list. The CRT's sprintf_s does.
	char buf[160];
	const double nyquist = r.maxFreqHz;

	const double deep = r.cutoffDeepHz;
	const double main = r.cutoffHz;

	if (deep <= 0.0) {
		out = "没有检测到信号";
		return out;
	}

	// The distinction that matters: a lossy encoder leaves a clean cliff - nothing
	// at all above its cutoff. Real material, lossless or not, keeps *something*
	// running up to Nyquist, and its music band usually ends well before that.
	// The numbers themselves are printed by the caller; this is just the verdict.
	const bool reachesTop = (deep >= nyquist * 0.95);
	const bool cleanCliff = (main > 0.0) && (deep <= main * 1.05);

	if (reachesTop) {
		out = "看起来是无损";
		return out;
	}

	if (cleanCliff) {
		const double khz = deep / 1000.0;
		if (deep >= 19500.0)      sprintf_s(buf, sizeof(buf), "%.1f kHz 以上没有内容 —— 常见于 320 kbps 有损", khz);
		else if (deep >= 17500.0) sprintf_s(buf, sizeof(buf), "%.1f kHz 以上没有内容 —— 常见于 192–256 kbps 有损", khz);
		else if (deep >= 14000.0) sprintf_s(buf, sizeof(buf), "%.1f kHz 以上没有内容 —— 常见于 128 kbps 有损", khz);
		else                      sprintf_s(buf, sizeof(buf), "%.1f kHz 以上没有内容 —— 带宽受限", khz);
		out = buf;
		return out;
	}

	// Content past the music band, but not all the way to Nyquist: neither a real
	// brick wall nor a full-bandwidth file.
	out = "切边不干净，无法定论";
	return out;
}

bool spectrogram_analyse(const char * path,
                         const t_spectrogram_params & params,
                         t_spectrogram_result & out,
                         abort_callback & abort,
                         const std::function<void(double)> & onProgress,
                         pfc::string8 & error) {
	out = t_spectrogram_result();
	error.reset();

	if (path == nullptr || *path == 0) {
		error = "没有给出文件路径";
		return false;
	}
	if (params.width == 0 || params.height == 0) {
		error = "输出尺寸不合法";
		return false;
	}

	try {
		input_decoder::ptr decoder;
		input_entry::g_open_for_decoding(decoder, nullptr, path, abort);
		decoder->initialize(0, 0, abort);

		file_info_impl info;
		decoder->get_info(0, info, abort);

		double duration = info.get_length();
		if (!(duration > 0.0)) {
			error = "拿不到时长（流式输入或损坏的文件），频谱图需要可定位的音源";
			return false;
		}

		// The first decoded chunk is the authoritative source for the sample rate
		// and channel count: file_info does not always carry the tech infos, and
		// a wrong sample rate would put every frequency label in the wrong place.
		audio_chunk_fast_impl firstChunk;
		const bool haveFirst = decoder->run(firstChunk, abort);

		unsigned sampleRate = 0;
		unsigned channels = 0;
		if (haveFirst) {
			sampleRate = firstChunk.get_srate();
			channels   = firstChunk.get_channels();
		}
		if (sampleRate == 0) sampleRate = (unsigned)info.info_get_int("samplerate");
		if (sampleRate == 0) sampleRate = 44100;
		if (channels == 0)   channels   = (unsigned)info.info_get_int("channels");

		CFft fft;
		if (!fft.init(params.fftSize)) {
			error = "FFT 长度必须是 2 的幂";
			return false;
		}
		const unsigned n = params.fftSize;
		const unsigned bins = n / 2;
		const unsigned hop = n / 2;             // 50% overlap
		const double refMag = 0.25 * (double)n; // full-scale sine lands at 0 dB

		std::vector<float> window(n);
		for (unsigned i = 0; i < n; ++i) {
			window[i] = (float)(0.5 * (1.0 - cos(2.0 * kPi * (double)i / (double)n)));
		}

		const double maxFreq = (double)sampleRate * 0.5;

		out.width = params.width;
		out.height = params.height;
		out.maxFreqHz = maxFreq;
		out.durationSec = duration;
		out.sampleRate = (double)sampleRate;
		out.channels = channels;
		out.cells.assign((size_t)params.width * params.height, 0);

		// Row -> frequency. Row 0 is DC, the top row is Nyquist.
		std::vector<double> rowFreq(params.height);
		for (unsigned y = 0; y < params.height; ++y) {
			rowFreq[y] = (params.height > 1)
				? maxFreq * (double)y / (double)(params.height - 1)
				: 0.0;
		}

		std::vector<float> re(n), im(n);
		std::vector<float> ring(n, 0.0f);
		unsigned ringPos = 0;
		unsigned long long samplesSeen = 0;
		unsigned sinceFrame = 0;

		std::vector<double> colTop(params.width, 0.0);
		//! Same vote but at the deep threshold: the file's real content edge, as
		//! opposed to where the music itself stops.
		std::vector<double> colTopDeep(params.width, 0.0);
		//! Last output column written; -1 before the first frame. Drives the
		//! column hold so short files do not leave blank stripes.
		long long lastCol = -1;
		//! One dB value per output row, reused for every frame: feeds both the
		//! image and the cutoff vote so the bins are only reduced once.
		std::vector<float> rowDb(params.height, -1000.0f);
		const unsigned long long totalSamples =
			(unsigned long long)(duration * (double)sampleRate);

		audio_chunk_fast_impl nextChunk;
		bool sawAnyAudio = false;

		// firstChunk was already pulled to learn the sample rate, so it has to be
		// processed before the rest of the stream.
		for (audio_chunk * pChunk = haveFirst ? &firstChunk : nullptr;
		     pChunk != nullptr;
		     pChunk = decoder->run(nextChunk, abort) ? &nextChunk : nullptr) {
			const audio_chunk & chunk = *pChunk;
			if (abort.is_aborting()) {
				error = "已取消";
				return false;
			}

			const unsigned count = (unsigned)chunk.get_sample_count();
			const unsigned chans = chunk.get_channels();
			const audio_sample * data = chunk.get_data();
			if (data == nullptr || count == 0 || chans == 0) continue;
			sawAnyAudio = true;

			for (unsigned i = 0; i < count; ++i) {
				float v = 0.0f;
				const audio_sample * frame = data + (size_t)i * chans;
				for (unsigned c = 0; c < chans; ++c) v += (float)frame[c];
				v /= (float)chans;

				ring[ringPos] = v;
				ringPos = (ringPos + 1) % n;
				++samplesSeen;
				++sinceFrame;

				if (samplesSeen < n || sinceFrame < hop) continue;
				sinceFrame = 0;

				for (unsigned k = 0; k < n; ++k) {
					re[k] = ring[(ringPos + k) % n] * window[k];
					im[k] = 0.0f;
				}
				fft.forward(re.data(), im.data());

				// Fold this frame into its output column.
				//
				// Frames arrive in time order, so the column never moves backwards.
				// Several frames can land in the same column - they are max-merged
				// below - and a forward jump leaves a gap, which gets the same data
				// (a hold, so a file shorter than the image is wide is not striped).
				//
				// NOTE: this must NOT force the column to advance by at least one
				// per frame. Doing that makes the picture consume one column per
				// frame, so any file with more frames than the image has columns
				// loses its tail entirely and the time axis comes out compressed -
				// a 3-minute track written into a 3000-column image simply stopped
				// drawing about 34 seconds before the end.
				const double t = (double)samplesSeen / (double)sampleRate;
				unsigned col = (unsigned)(t / duration * (double)params.width);
				if (col >= params.width) col = params.width - 1;
				if (lastCol >= 0 && col < (unsigned)lastCol) col = (unsigned)lastCol;
				const unsigned fromCol = (lastCol >= 0) ? (unsigned)lastCol : 0;

				// Reduce the frame to one dB value per output row.
				float framePeakDb = -1000.0f;
				for (unsigned y = 0; y < params.height; ++y) {
					const double fLo = rowFreq[y];
					const double fHi = (y + 1 < params.height) ? rowFreq[y + 1] : maxFreq;
					long b0 = (long)floor(fLo / maxFreq * (double)bins);
					long b1 = (long)ceil (fHi / maxFreq * (double)bins);
					if (b0 < 0) b0 = 0;
					if (b0 > (long)bins - 1) b0 = (long)bins - 1;
					if (b1 <= b0) b1 = b0 + 1;
					if (b1 > (long)bins) b1 = (long)bins;

					float best = 0.0f;
					for (long b = b0; b < b1; ++b) {
						const float m = sqrtf(re[b] * re[b] + im[b] * im[b]);
						if (m > best) best = m;
					}

					const float db = 20.0f * log10f(best / (float)refMag + 1.0e-12f);
					rowDb[y] = db;
					if (db > framePeakDb) framePeakDb = db;
				}

				// Cutoff vote, twice: the highest row still within contentDropDb of
				// this frame's own peak (where the music ends) and the highest row
				// within deepDropDb (where the file's content ends). Both adapt to
				// level instead of needing a hand-tuned absolute threshold.
				unsigned topRowMain = 0, topRowDeep = 0;
				const bool votes = (framePeakDb > (float)params.activeFloorDb);
				if (votes) {
					const float thrMain = framePeakDb - (float)params.contentDropDb;
					const float thrDeep = framePeakDb - (float)params.deepDropDb;
					for (unsigned y = 0; y < params.height; ++y) {
						if (rowDb[y] > thrMain) topRowMain = y;
						if (rowDb[y] > thrDeep) topRowDeep = y;
					}
				}

				// Store the level, not a colour.
				//
				// Clamp *before* the cast: the expression goes negative for
				// anything below the store floor, and casting a negative double to
				// unsigned is undefined - MSVC wrapped it to a huge value, which a
				// following ">255" test then clamped to the top of the scale, so
				// every silent region came out at full brightness.
				for (unsigned c = fromCol; c <= col; ++c) {
					unsigned char * cells = out.cells.data() + (size_t)c;
					for (unsigned y = 0; y < params.height; ++y) {
						double q = (rowDb[y] + kCellStoreRangeDb) / kCellStoreRangeDb * 255.0;
						if (!(q > 0.0)) q = 0.0;   // also catches NaN
						if (q > 255.0) q = 255.0;
						const unsigned char v = (unsigned char)q;
						unsigned char & cell = cells[(size_t)y * params.width];
						if (v > cell) cell = v;
					}
					if (votes) {
						const double hzMain = rowFreq[topRowMain];
						const double hzDeep = rowFreq[topRowDeep];
						if (hzMain > colTop[c]) colTop[c] = hzMain;
						if (hzDeep > colTopDeep[c]) colTopDeep[c] = hzDeep;
					}
				}
				lastCol = (long long)col;
			}

			if (onProgress && (samplesSeen % (unsigned long long)(sampleRate / 2) < count)) {
				double frac = (totalSamples > 0)
					? (double)samplesSeen / (double)totalSamples : 0.0;
				if (frac > 1.0) frac = 1.0;
				onProgress(frac);
			}
		}

		if (!sawAnyAudio) {
			error = "解码器没有返回任何音频数据";
			return false;
		}

		// NOTE: no per-file brightness normalisation here, deliberately.
		//
		// An earlier revision rescaled the whole image so its loudest cell reached
		// 255. That looks tidy but destroys the one thing a spectrogram is for:
		// brightness has to mean level. It pushed content that sits 40-70 dB below
		// the peak (codec noise, a quiet recording's high end) up into the top of
		// the palette, so regions with essentially nothing in them came out solid
		// red. The mapping below is absolute instead: 0 dBFS is the top of the
		// scale, silence is black, and the picture can be read at face value.

		// Median of the per-column tops - one loud frame should not drag the
		// estimate up, and one silent column should not drag it down.
		auto median_of = [](const std::vector<double> & v) -> double {
			std::vector<double> tops;
			tops.reserve(v.size());
			for (size_t i = 0; i < v.size(); ++i) {
				if (v[i] > 0.0) tops.push_back(v[i]);
			}
			if (tops.empty()) return 0.0;
			std::sort(tops.begin(), tops.end());
			return tops[tops.size() / 2];
		};

		unsigned withContent = 0;
		for (unsigned i = 0; i < params.width; ++i) {
			if (colTop[i] > 0.0) ++withContent;
		}
		out.columnsTotal = params.width;
		out.columnsWithContent = withContent;
		out.cutoffHz     = median_of(colTop);
		out.cutoffDeepHz = median_of(colTopDeep);

		if (onProgress) onProgress(1.0);
		return true;
	}
	catch (const std::exception & e) {
		error = "解码失败：";
		error += e.what();
		return false;
	}
	catch (...) {
		error = "解码失败（未知错误）";
		return false;
	}
}
