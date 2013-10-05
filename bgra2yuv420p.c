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

static uint8_t avg(uint16_t a0, uint16_t a1) {
	return (uint16_t)(a0 + a1 + 1u) >> 1;
}

static uint8_t avg1(uint8_t a00, uint8_t a10, uint8_t a01, uint8_t a11) {
	return ~avg(~avg(a00, a10), ~avg(a01, a11));
}

static void bgra2yuv420p_2x2(const uint8_t *restrict p0, const uint8_t *restrict p1,
	uint8_t *restrict y0, uint8_t *restrict y1, uint8_t *restrict u, uint8_t *restrict v) {
	y0[0] = (uint16_t)(Ybias + KBYi * p0[0] + KGYi * p0[1] + KRYi * p0[2]) >> SY;
	y0[1] = (uint16_t)(Ybias + KBYi * p0[4] + KGYi * p0[5] + KRYi * p0[6]) >> SY;
	y1[0] = (uint16_t)(Ybias + KBYi * p1[0] + KGYi * p1[1] + KRYi * p1[2]) >> SY;
	y1[1] = (uint16_t)(Ybias + KBYi * p1[4] + KGYi * p1[5] + KRYi * p1[6]) >> SY;

	const uint8_t b = avg1(p0[0], p1[0], p0[4], p1[4]);
	const uint8_t g = avg1(p0[1], p1[1], p0[5], p1[5]);
	const uint8_t r = avg1(p0[2], p1[2], p0[6], p1[6]);

	*u = (uint16_t)(Cbias + KBUi * b + KGUi * g + KRUi * r) >> SC;
	*v = (uint16_t)(Cbias + KBVi * b + KGVi * g + KRVi * r) >> SC;
}

#if defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__)) && defined(__SSSE3__)

#include <tmmintrin.h>

static __m128i avg32(const __m128i p0[2], const __m128i p1[2]) {
	const __m128i I = _mm_set1_epi8(~0);

	const __m128 a0 = _mm_castsi128_ps(_mm_xor_si128(_mm_avg_epu8(p0[0], p1[0]), I));
	const __m128 a1 = _mm_castsi128_ps(_mm_xor_si128(_mm_avg_epu8(p0[1], p1[1]), I));

	return _mm_xor_si128(_mm_avg_epu8(_mm_castps_si128(_mm_shuffle_ps(a0, a1, _MM_SHUFFLE(2, 0, 2, 0))),
			_mm_castps_si128(_mm_shuffle_ps(a0, a1, _MM_SHUFFLE(3, 1, 3, 1)))), I);
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
		avg32(p0 + 0, p1 + 0), avg32(p0 + 2, p1 + 2),
		avg32(p0 + 4, p1 + 4), avg32(p0 + 6, p1 + 6)
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
}

#else

void bgra2yuv420p(const uint8_t *restrict bgra, ptrdiff_t bgra_stride,
	uint8_t *restrict y, ptrdiff_t y_stride, uint8_t *restrict u, ptrdiff_t u_stride,
	uint8_t *restrict v, ptrdiff_t v_stride, size_t width, size_t height) {
	const size_t step2 = width / 2;

	for (size_t row2 = 0; row2 < height / 2; row2++) {
		const uint8_t *p0 = bgra;
		uint8_t *y0 = y, *u0 = u, *v0 = v;

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
}

#endif
