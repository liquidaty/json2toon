/* json2toon - runtime-selected character-classification scanners.
 *
 * All architecture- and compiler-specific constructs (intrinsics, feature
 * macros) are centralized here. Each vectorized routine is paired with a scalar
 * routine of identical semantics; the fastest available variant is bound to a
 * function pointer at startup, and a correct scalar fallback exists for every
 * target. Adding or removing an instruction set touches only this file.
 */
#include "internal.h"

#if !defined(J2T_DISABLE_SIMD)
#  if defined(J2T_HAVE_NEON) || (defined(__aarch64__) && defined(__ARM_NEON))
#    include <arm_neon.h>
#    define J2T_USE_NEON 1
#  elif defined(J2T_HAVE_SSE2) || defined(__SSE2__) || \
        (defined(_M_X64) && !defined(_M_ARM64))
#    include <emmintrin.h>
#    define J2T_USE_SSE2 1
#  endif
#endif

/* ------------------------------------------------------------------ scalar */

static const char *skip_ws_scalar(const char *p, const char *end) {
  for (; p < end; p++) {
    char c = *p;
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
      break;
  }
  return p;
}

static const char *scan_string_scalar(const char *p, const char *end) {
  for (; p < end; p++) {
    unsigned char c = (unsigned char)*p;
    if (c == '"' || c == '\\' || c < 0x20)
      break;
  }
  return p;
}

/* --------------------------------------------------------------- SSE2 (x86) */

#if defined(J2T_USE_SSE2)
static const char *skip_ws_sse2(const char *p, const char *end) {
  while (end - p >= 16) {
    __m128i v = _mm_loadu_si128((const __m128i *)p);
    __m128i ws = _mm_or_si128(
        _mm_or_si128(_mm_cmpeq_epi8(v, _mm_set1_epi8(' ')),
                     _mm_cmpeq_epi8(v, _mm_set1_epi8('\t'))),
        _mm_or_si128(_mm_cmpeq_epi8(v, _mm_set1_epi8('\n')),
                     _mm_cmpeq_epi8(v, _mm_set1_epi8('\r'))));
    /* bits set where byte is NOT whitespace -> a stopping position */
    unsigned mask = (~(unsigned)_mm_movemask_epi8(ws)) & 0xffffu;
    if (mask)
      return p + __builtin_ctz(mask);
    p += 16;
  }
  return skip_ws_scalar(p, end);
}

static const char *scan_string_sse2(const char *p, const char *end) {
  while (end - p >= 16) {
    __m128i v = _mm_loadu_si128((const __m128i *)p);
    __m128i ctrl = _mm_cmpeq_epi8(_mm_subs_epu8(v, _mm_set1_epi8(0x1f)),
                                  _mm_setzero_si128());
    __m128i stop = _mm_or_si128(
        _mm_or_si128(_mm_cmpeq_epi8(v, _mm_set1_epi8('"')),
                     _mm_cmpeq_epi8(v, _mm_set1_epi8('\\'))),
        ctrl);
    unsigned mask = (unsigned)_mm_movemask_epi8(stop) & 0xffffu;
    if (mask)
      return p + __builtin_ctz(mask);
    p += 16;
  }
  return scan_string_scalar(p, end);
}
#endif

/* -------------------------------------------------------------- NEON (ARM) */

#if defined(J2T_USE_NEON)
static const char *skip_ws_neon(const char *p, const char *end) {
  while (end - p >= 16) {
    uint8x16_t v = vld1q_u8((const uint8_t *)p);
    uint8x16_t ws = vorrq_u8(
        vorrq_u8(vceqq_u8(v, vdupq_n_u8(' ')), vceqq_u8(v, vdupq_n_u8('\t'))),
        vorrq_u8(vceqq_u8(v, vdupq_n_u8('\n')), vceqq_u8(v, vdupq_n_u8('\r'))));
    /* stop lanes = 0xFF where NOT whitespace */
    uint8x16_t stop = vmvnq_u8(ws);
    if (vmaxvq_u8(stop))
      return skip_ws_scalar(p, p + 16);
    p += 16;
  }
  return skip_ws_scalar(p, end);
}

static const char *scan_string_neon(const char *p, const char *end) {
  while (end - p >= 16) {
    uint8x16_t v = vld1q_u8((const uint8_t *)p);
    uint8x16_t ctrl = vcltq_u8(v, vdupq_n_u8(0x20));
    uint8x16_t stop = vorrq_u8(
        vorrq_u8(vceqq_u8(v, vdupq_n_u8('"')), vceqq_u8(v, vdupq_n_u8('\\'))),
        ctrl);
    if (vmaxvq_u8(stop))
      return scan_string_scalar(p, p + 16);
    p += 16;
  }
  return scan_string_scalar(p, end);
}
#endif

/* ----------------------------------------------------------------- dispatch */

/* The backend is fixed at compile time -- the #if block at the top of this file
 * compiles in exactly one variant -- so the scanners are bound here at static-
 * initialization time. There is no runtime mutation of these globals and thus
 * no init handshake and no data race when converters are created concurrently
 * from multiple threads. */
#if defined(J2T_USE_NEON)
j2t_scan_fn j2t_skip_ws = skip_ws_neon;
j2t_scan_fn j2t_scan_string = scan_string_neon;
static const char *const g_backend = "neon";
#elif defined(J2T_USE_SSE2)
j2t_scan_fn j2t_skip_ws = skip_ws_sse2;
j2t_scan_fn j2t_scan_string = scan_string_sse2;
static const char *const g_backend = "sse2";
#else
j2t_scan_fn j2t_skip_ws = skip_ws_scalar;
j2t_scan_fn j2t_scan_string = scan_string_scalar;
static const char *const g_backend = "scalar";
#endif

const char *j2t_simd_backend(void) { return g_backend; }
