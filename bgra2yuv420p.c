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
#include <string.h>

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

#include <tmmintrin.h>

static void bgra2yuv420p_2x2(const void *restrict bgra0, const void *restrict bgra1,
	void *restrict y0, void *restrict y1, void *restrict u, void *restrict v) {
	const __m64 Y = _mm_set_pi8(0, KRYi, KGYi, KBYi, 0, KRYi, KGYi, KBYi);
	const __m64 Yb = _mm_set1_pi16(Ybias);

	const __m64 U = _mm_set_pi8(0, 0, 0, 0, 0, KRUi, KGUi, KBUi);
	const __m64 V = _mm_set_pi8(0, 0, 0, 0, 0, KRVi, KGVi, KBVi);
	const __m64 Cb = _mm_set_pi16(0, Cbias, 0, Cbias);

	const __m64 p0 = *(const __m64 *)bgra0;
	const __m64 p1 = *(const __m64 *)bgra1;

	const __m64 a01 = _mm_avg_pu8(p0, p1);
	const __m64 aa = _mm_avg_pu8(a01, _mm_shuffle_pi16(a01, _MM_SHUFFLE(1, 0, 3, 2)));

	const __m64 yyyy = _mm_add_pi16(Yb, _mm_hadd_pi16(_mm_maddubs_pi16(p0, Y), _mm_maddubs_pi16(p1, Y)));
	const __m64 u0v0 = _mm_add_pi16(Cb, _mm_hadd_pi16(_mm_maddubs_pi16(aa, U), _mm_maddubs_pi16(aa, V)));
	const __m64 yyyyu0v0 = _mm_packs_pu16(_mm_srli_pi16(yyyy, SY), _mm_srli_pi16(u0v0, SC));

	*(uint16_t *)y0 = _mm_extract_pi16(yyyyu0v0, 0);
	*(uint16_t *)y1 = _mm_extract_pi16(yyyyu0v0, 1);
	*(uint8_t *)u = _mm_extract_pi16(yyyyu0v0, 2);
	*(uint8_t *)v = _mm_extract_pi16(yyyyu0v0, 3);
}

static __m128i avg(const __m128i p0[2], const __m128i p1[2]) {
	const __m128 a0 = _mm_castsi128_ps(_mm_avg_epu8(p0[0], p1[0]));
	const __m128 a1 = _mm_castsi128_ps(_mm_avg_epu8(p0[1], p1[1]));
	return _mm_avg_epu8(_mm_castps_si128(_mm_shuffle_ps(a0, a1, _MM_SHUFFLE(2, 0, 2, 0))),
		_mm_castps_si128(_mm_shuffle_ps(a0, a1, _MM_SHUFFLE(3, 1, 3, 1))));
}

static void dot(void *c, const __m128i p[4], __m128i K, __m128i B, int S) {
	const __m128i c0 = _mm_add_epi16(B, _mm_hadd_epi16(_mm_maddubs_epi16(p[0], K), _mm_maddubs_epi16(p[1], K)));
	const __m128i c2 = _mm_add_epi16(B, _mm_hadd_epi16(_mm_maddubs_epi16(p[2], K), _mm_maddubs_epi16(p[3], K)));
	_mm_storeu_si128(c, _mm_packus_epi16(_mm_srli_epi16(c0, S), _mm_srli_epi16(c2, S)));
}

static void bgra2yuv420p_32x2(const void *restrict bgra0, const void *restrict bgra1,
	void *restrict y0, void *restrict y1, void *restrict u, void *restrict v) {
	const __m128i Y = _mm_set_epi8(0, KRYi, KGYi, KBYi, 0, KRYi, KGYi, KBYi,
		0, KRYi, KGYi, KBYi, 0, KRYi, KGYi, KBYi);
	const __m128i Yb = _mm_set1_epi16(Ybias);

	const __m128i U = _mm_set_epi8(0, KRUi, KGUi, KBUi, 0, KRUi, KGUi, KBUi,
		0, KRUi, KGUi, KBUi, 0, KRUi, KGUi, KBUi);
	const __m128i V = _mm_set_epi8(0, KRVi, KGVi, KBVi, 0, KRVi, KGVi, KBVi,
		0, KRVi, KGVi, KBVi, 0, KRVi, KGVi, KBVi);
	const __m128i Cb = _mm_set1_epi16(Cbias);

	__m128i p0[8], p1[8];
	memcpy(p0, bgra0, sizeof(p0));
	memcpy(p1, bgra1, sizeof(p1));

	dot((__m128i *)y0 + 0, p0 + 0, Y, Yb, SY);
	dot((__m128i *)y0 + 1, p0 + 4, Y, Yb, SY);
	dot((__m128i *)y1 + 0, p1 + 0, Y, Yb, SY);
	dot((__m128i *)y1 + 1, p1 + 4, Y, Yb, SY);

	const __m128i a[4] = {
		avg(p0 + 0, p1 + 0), avg(p0 + 2, p1 + 2),
		avg(p0 + 4, p1 + 4), avg(p0 + 6, p1 + 6)
	};

	dot(u, a, U, Cb, SC);
	dot(v, a, V, Cb, SC);
}

void bgra2yuv420p(const uint8_t *restrict bgra, ptrdiff_t bgra_stride,
	uint8_t *restrict y, ptrdiff_t y_stride, uint8_t *restrict u, ptrdiff_t u_stride,
	uint8_t *restrict v, ptrdiff_t v_stride, size_t width, size_t height) {
	const size_t step32 = width / 32;
	const size_t step2 = width % 32 / 2;

	for (size_t row2 = 0; row2 < height / 2; row2++) {
		const uint8_t *p0 = bgra;
		uint8_t *y0 = y, *u0 = u, *v0 = v;

		for (size_t i = 0; i < step32; i++) {
			bgra2yuv420p_32x2(p0, p0 + bgra_stride, y0, y0 + y_stride, u0, v0);
			p0 += 32 * 4;
			y0 += 32;
			u0 += 16;
			v0 += 16;
		}

		for (size_t i = 0; i < step2; i++) {
			bgra2yuv420p_2x2(p0, p0 + bgra_stride, y0, y0 + y_stride, u0, v0);
			p0 += 2 * 4;
			y0 += 2;
			u0 += 1;
			v0 += 1;
		}

		bgra += bgra_stride * 2;
		y += y_stride * 2;
		u += u_stride;
		v += v_stride;
	}

	_mm_empty();
}

#else

void bgra2yuv420p(const uint8_t *restrict bgra, ptrdiff_t bgra_stride,
	uint8_t *restrict y, ptrdiff_t y_stride, uint8_t *restrict u, ptrdiff_t u_stride,
	uint8_t *restrict v, ptrdiff_t v_stride, size_t width, size_t height) {
	for (size_t row2 = 0; row2 < height / 2; row2++) {
		const uint8_t *restrict p0 = bgra;
		uint8_t *restrict y0 = y, *restrict u0 = u, *restrict v0 = v;

		for (size_t col2 = 0; col2 < width / 2; col2++) {
			const uint8_t *restrict p1 = p0 + bgra_stride;
			uint8_t *restrict y1 = y0 + y_stride;

			const uint8_t b00 = p0[0], g00 = p0[1], r00 = p0[2], b01 = p0[4], g01 = p0[5], r01 = p0[6];
			const uint8_t b10 = p1[0], g10 = p1[1], r10 = p1[2], b11 = p1[4], g11 = p1[5], r11 = p1[6];

			y0[0] = (uint16_t)(Ybias + KRYi * r00 + KGYi * g00 + KBYi * b00) >> SY;
			y0[1] = (uint16_t)(Ybias + KRYi * r01 + KGYi * g01 + KBYi * b01) >> SY;
			y1[0] = (uint16_t)(Ybias + KRYi * r10 + KGYi * g10 + KBYi * b10) >> SY;
			y1[1] = (uint16_t)(Ybias + KRYi * r11 + KGYi * g11 + KBYi * b11) >> SY;

			const uint8_t r = (uint16_t)(r00 + r01 + r10 + r11) >> 2;
			const uint8_t g = (uint16_t)(g00 + g01 + g10 + g11) >> 2;
			const uint8_t b = (uint16_t)(b00 + b01 + b10 + b11) >> 2;

			*u0 = (uint16_t)(Cbias + KRUi * r + KGUi * g + KBUi * b) >> SC;
			*v0 = (uint16_t)(Cbias + KRVi * r + KGVi * g + KBVi * b) >> SC;

			p0 += 2 * 4;
			y0 += 2;
			u0 += 1;
			v0 += 1;
		}

		bgra += bgra_stride * 2;
		y += y_stride * 2;
		u += u_stride;
		v += v_stride;
	}
}

#endif
