/* Fast BGRA -> YUV420p conversion
 *
 * Copyright 2013 Constantin Baranov
 *
 * This file is part of GLGrab.
 *
 * GLGrab is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * GLGrab is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GLGrab.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "bgra2yuv420p.h"

#define BT_709_KB 0.0722
#define BT_709_KR 0.2126

#define BT_601_KB 0.114
#define BT_601_KR 0.299

#define KB BT_709_KB
#define KR BT_709_KR

#define KG (1.0 - KB - KR)
#define KY (220.0 / 256.0)
#define KC (112.0 / 256.0)

#define KRY (KY * KR)
#define KGY (KY * KG)
#define KBY (KY * KB)
#define KRU (KC * KR / (KB - 1.0))
#define KGU (KC * KG / (KB - 1.0))
#define KBU KC
#define KRV KC
#define KGV (KC * KG / (KR - 1.0))
#define KBV (KC * KB / (KR - 1.0))

#define SY 7
#define SC 7

static const int16_t Ybias = (16 << SY) + (1 << (SY - 1));
static const int8_t KRYi = KRY * (1 << SY) + 0.5;
static const int8_t KGYi = KGY * (1 << SY) + 0.5;
static const int8_t KBYi = KBY * (1 << SY) + 0.5;

static const int16_t Cbias = (128 << SC) + (1 << (SC - 1));
static const int8_t KRUi = KRU * (1 << SC) - 0.5;
static const int8_t KGUi = KGU * (1 << SC) - 0.5;
static const int8_t KBUi = KBU * (1 << SC) + 0.5;
static const int8_t KRVi = KRV * (1 << SC) + 0.5;
static const int8_t KGVi = KGV * (1 << SC) - 0.5;
static const int8_t KBVi = KBV * (1 << SC) - 0.5;

#if defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__)) && defined(__SSSE3__)

#include <x86intrin.h>

static inline __m128i navg(__m128i x, __m128i y) {
	return _mm_xor_si128(_mm_avg_epu8(x, y), _mm_set1_epi8(-1));
}

static inline __m128i avg(__m128i n0123, __m128i n4567) {
	const __m128i n0415 = _mm_unpacklo_epi32(n0123, n4567);
	const __m128i n2637 = _mm_unpackhi_epi32(n0123, n4567);
	const __m128i n0246 = _mm_unpacklo_epi32(n0415, n2637);
	const __m128i n1357 = _mm_unpackhi_epi32(n0415, n2637);
	return navg(n0246, n1357);
}

static inline __m128i dot(__m128i p0, __m128i p1, __m128i K) {
	const __m128i d0 = _mm_maddubs_epi16(p0, K);
	const __m128i d1 = _mm_maddubs_epi16(p1, K);
	return _mm_hadd_epi16(d0, d1);
}

static inline __m128i pack(__m128i d0, __m128i d1, __m128i B, int S) {
	d0 = _mm_srli_epi16(_mm_add_epi16(d0, B), S);
	d1 = _mm_srli_epi16(_mm_add_epi16(d1, B), S);
	return _mm_packus_epi16(d0, d1);
}

static inline __m128i load(const __m128i *v, ptrdiff_t i) {
#if defined(__SSE4_1__)
	return _mm_stream_load_si128((__m128i *)v + i);
#else
	return _mm_load_si128(v + i);
#endif
}

static inline __m128i bgra2yuv420p_16x2(const void *restrict bgra0, const void *restrict bgra1,
	void *restrict y0, void *restrict y1) {
	const __m128i Y = _mm_set_epi8(0, KRYi, KGYi, KBYi, 0, KRYi, KGYi, KBYi,
		0, KRYi, KGYi, KBYi, 0, KRYi, KGYi, KBYi);
	const __m128i Yb = _mm_set1_epi16(Ybias);

	const __m128i U = _mm_set_epi8(0, KRUi, KGUi, KBUi, 0, KRUi, KGUi, KBUi,
		0, KRUi, KGUi, KBUi, 0, KRUi, KGUi, KBUi);
	const __m128i V = _mm_set_epi8(0, KRVi, KGVi, KBVi, 0, KRVi, KGVi, KBVi,
		0, KRVi, KGVi, KBVi, 0, KRVi, KGVi, KBVi);
	const __m128i Cb = _mm_set1_epi16(Cbias);

	const __m128i p00 = load(bgra0, 0);
	const __m128i p01 = load(bgra0, 1);
	const __m128i d00 = dot(p00, p01, Y);

	const __m128i p10 = load(bgra1, 0);
	const __m128i p11 = load(bgra1, 1);
	const __m128i n0 = navg(p00, p10);
	const __m128i n1 = navg(p01, p11);
	const __m128i d10 = dot(p10, p11, Y);
	const __m128i a01 = avg(n0, n1);

	const __m128i p02 = load(bgra0, 2);
	const __m128i p03 = load(bgra0, 3);
	const __m128i d02 = dot(p02, p03, Y);

	_mm_stream_si128(y0, pack(d00, d02, Yb, SY));

	const __m128i p12 = load(bgra1, 2);
	const __m128i p13 = load(bgra1, 3);
	const __m128i n2 = navg(p02, p12);
	const __m128i n3 = navg(p03, p13);
	const __m128i d12 = dot(p12, p13, Y);
	const __m128i a23 = avg(n2, n3);

	_mm_stream_si128(y1, pack(d10, d12, Yb, SY));

	return pack(dot(a01, a23, U), dot(a01, a23, V), Cb, SC);
}

static inline void bgra2yuv420p_32x2(const uint8_t *restrict bgra0, const uint8_t *restrict bgra1,
	uint8_t *restrict y0, uint8_t *restrict y1, void *restrict u, void *restrict v) {
	const __m128i uv0 = bgra2yuv420p_16x2(bgra0 + 0x00, bgra1 + 0x00, y0 + 0x00, y1 + 0x00);
	const __m128i uv1 = bgra2yuv420p_16x2(bgra0 + 0x40, bgra1 + 0x40, y0 + 0x10, y1 + 0x10);

	_mm_stream_si128(u, _mm_unpacklo_epi64(uv0, uv1));
	_mm_stream_si128(v, _mm_unpackhi_epi64(uv0, uv1));
}

void bgra2yuv420p(const uint8_t *restrict bgra, uint8_t *restrict yuv, size_t width32, size_t height2) {
	const size_t count_2x2 = width32 * 16 * height2;
	const ptrdiff_t bgra_stride = width32 * 128;
	const ptrdiff_t y_stride = width32 * 32;

	const uint8_t *p0 = bgra + bgra_stride * (height2 * 2 - 1);
	uint8_t *y0 = yuv;
	uint8_t *u = y0 + count_2x2 * 4;
	uint8_t *v = u + count_2x2;

	for (size_t row2 = 0; row2 < height2; row2++) {
		const uint8_t *p1 = p0 - bgra_stride;
		uint8_t *y1 = y0 + y_stride;

		for (size_t i = 0; i < width32; i++) {
			bgra2yuv420p_32x2(p0, p1, y0, y1, u, v);

			p0 += 128;
			p1 += 128;
			y0 += 32;
			y1 += 32;
			u += 16;
			v += 16;
		}

		p0 = p1 - bgra_stride * 2;
		y0 += y_stride;
	}

	_mm_sfence();
}

#else

static uint8_t navg(uint16_t a0, uint16_t a1) {
	return ~(uint16_t)(a0 + a1) >> 1;
}

static uint8_t avg(uint8_t a00, uint8_t a10, uint8_t a01, uint8_t a11) {
	return navg(navg(a00, a10), navg(a01, a11));
}

static void bgra2yuv420p_2x2(const uint8_t *restrict p0, const uint8_t *restrict p1,
	uint8_t *restrict y0, uint8_t *restrict y1, uint8_t *restrict u, uint8_t *restrict v) {
	y0[0] = (uint16_t)(Ybias + KBYi * p0[0] + KGYi * p0[1] + KRYi * p0[2]) >> SY;
	y0[1] = (uint16_t)(Ybias + KBYi * p0[4] + KGYi * p0[5] + KRYi * p0[6]) >> SY;
	y1[0] = (uint16_t)(Ybias + KBYi * p1[0] + KGYi * p1[1] + KRYi * p1[2]) >> SY;
	y1[1] = (uint16_t)(Ybias + KBYi * p1[4] + KGYi * p1[5] + KRYi * p1[6]) >> SY;

	const uint8_t b = avg(p0[0], p1[0], p0[4], p1[4]);
	const uint8_t g = avg(p0[1], p1[1], p0[5], p1[5]);
	const uint8_t r = avg(p0[2], p1[2], p0[6], p1[6]);

	*u = (uint16_t)(Cbias + KBUi * b + KGUi * g + KRUi * r) >> SC;
	*v = (uint16_t)(Cbias + KBVi * b + KGVi * g + KRVi * r) >> SC;
}

void __attribute__((noinline, optimize("tree-vectorize")))
bgra2yuv420p(const uint8_t *restrict bgra, uint8_t *restrict yuv, size_t width32, size_t height2) {
	const size_t count_2x2 = width32 * 16 * height2;
	const ptrdiff_t bgra_stride = width32 * 128;
	const ptrdiff_t y_stride = width32 * 32;

	const uint8_t *p0 = bgra + bgra_stride * (height2 * 2 - 1);
	uint8_t *y0 = yuv;
	uint8_t *u = y0 + count_2x2 * 4;
	uint8_t *v = u + count_2x2;

	for (size_t row2 = 0; row2 < height2; row2++) {
		const uint8_t *p1 = p0 - bgra_stride;
		uint8_t *y1 = y0 + y_stride;

		for (size_t i = 0; i < width32 * 16; i++) {
			bgra2yuv420p_2x2(p0, p1, y0, y1, u, v);

			p0 += 8;
			p1 += 8;
			y0 += 2;
			y1 += 2;
			u += 1;
			v += 1;
		}

		p0 = p1 - bgra_stride * 2;
		y0 += y_stride;
	}
}

#endif
