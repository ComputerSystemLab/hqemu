/* Native implementation of soft float functions */
#ifndef SOFTFLOAT_NATIVE_H
#define SOFTFLOAT_NATIVE_H

#include "softfloat-common.h"

#include <math.h>

#if (defined(CONFIG_BSD) && !defined(__APPLE__) && !defined(__GLIBC__)) \
    || defined(CONFIG_SOLARIS)
#include <ieeefp.h>
#define fabsf(f) ((float)fabs(f))
#else
#include <fenv.h>
#endif

#if defined(__OpenBSD__) || defined(__NetBSD__)
#include <sys/param.h>
#endif

/*
 * Define some C99-7.12.3 classification macros and
 *        some C99-.12.4 for Solaris systems OS less than 10,
 *        or Solaris 10 systems running GCC 3.x or less.
 *   Solaris 10 with GCC4 does not need these macros as they
 *   are defined in <iso/math_c99.h> with a compiler directive
 */
#if defined(CONFIG_SOLARIS) && \
           ((CONFIG_SOLARIS_VERSION <= 9 ) || \
           ((CONFIG_SOLARIS_VERSION == 10) && (__GNUC__ < 4))) \
    || (defined(__OpenBSD__) && (OpenBSD < 200811))
/*
 * C99 7.12.3 classification macros
 * and
 * C99 7.12.14 comparison macros
 *
 * ... do not work on Solaris 10 using GNU CC 3.4.x.
 * Try to workaround the missing / broken C99 math macros.
 */
#if defined(__OpenBSD__)
#define unordered(x, y) (isnan(x) || isnan(y))
#endif

#ifdef __NetBSD__
#ifndef isgreater
#define isgreater(x, y)		__builtin_isgreater(x, y)
#endif
#ifndef isgreaterequal
#define isgreaterequal(x, y)	__builtin_isgreaterequal(x, y)
#endif
#ifndef isless
#define isless(x, y)		__builtin_isless(x, y)
#endif
#ifndef islessequal
#define islessequal(x, y)	__builtin_islessequal(x, y)
#endif
#ifndef isunordered
#define isunordered(x, y)	__builtin_isunordered(x, y)
#endif
#endif


#define isnormal(x)             (fpclass(x) >= FP_NZERO)
#define isgreater(x, y)         ((!unordered(x, y)) && ((x) > (y)))
#define isgreaterequal(x, y)    ((!unordered(x, y)) && ((x) >= (y)))
#define isless(x, y)            ((!unordered(x, y)) && ((x) < (y)))
#define islessequal(x, y)       ((!unordered(x, y)) && ((x) <= (y)))
#define isunordered(x,y)        unordered(x, y)
#endif

#if defined(__sun__) && !defined(CONFIG_NEEDS_LIBSUNMATH)

#ifndef isnan
# define isnan(x) \
    (sizeof (x) == sizeof (long double) ? isnan_ld (x) \
     : sizeof (x) == sizeof (double) ? isnan_d (x) \
     : isnan_f (x))
static inline int isnan_f  (float       x) { return x != x; }
static inline int isnan_d  (double      x) { return x != x; }
static inline int isnan_ld (long double x) { return x != x; }
#endif

#ifndef isinf
# define isinf(x) \
    (sizeof (x) == sizeof (long double) ? isinf_ld (x) \
     : sizeof (x) == sizeof (double) ? isinf_d (x) \
     : isinf_f (x))
static inline int isinf_f  (float       x) { return isnan (x - x); }
static inline int isinf_d  (double      x) { return isnan (x - x); }
static inline int isinf_ld (long double x) { return isnan (x - x); }
#endif
#endif

typedef float float32;
typedef double float64;
#ifdef FLOATX80
typedef long double floatx80;
#endif

typedef union {
    float32 f;
    uint32_t i;
} float32u;
typedef union {
    float64 f;
    uint64_t i;
} float64u;
#ifdef FLOATX80
typedef union {
    floatx80 f;
    struct {
        uint64_t low;
        uint16_t high;
    } i;
} floatx80u;
#endif

/*----------------------------------------------------------------------------
| Software IEC/IEEE floating-point rounding mode.
*----------------------------------------------------------------------------*/
#if (defined(CONFIG_BSD) && !defined(__APPLE__) && !defined(__GLIBC__)) \
    || defined(CONFIG_SOLARIS)
#if defined(__OpenBSD__)
#define FE_RM FP_RM
#define FE_RP FP_RP
#define FE_RZ FP_RZ
#endif
enum {
    float_round_nearest_even = FP_RN,
    float_round_down         = FP_RM,
    float_round_up           = FP_RP,
    float_round_to_zero      = FP_RZ
};
#else
enum {
    float_round_nearest_even = FE_TONEAREST,
    float_round_down         = FE_DOWNWARD,
    float_round_up           = FE_UPWARD,
    float_round_to_zero      = FE_TOWARDZERO
};
#endif

typedef struct float_status {
    int float_rounding_mode;
#ifdef FLOATX80
    int floatx80_rounding_precision;
#endif
} float_status;

/* XXX: this code implements the x86 behaviour, not the IEEE one.  */
#if HOST_LONG_BITS == 32
static inline int long_to_int32(long a)
{
    return a;
}
#else
static inline int long_to_int32(long a)
{
    if (a != (int32_t)a)
        a = 0x80000000;
    return a;
}
#endif

INLINE void set_float_rounding_mode(int val STATUS_PARAM)
{
    STATUS(float_rounding_mode) = val;
#if (defined(CONFIG_BSD) && !defined(__APPLE__) && !defined(__GLIBC__)) || \
    (defined(CONFIG_SOLARIS) && CONFIG_SOLARIS_VERSION < 10)
    fpsetround(val);
#else
    fesetround(val);
#endif
}

#ifdef FLOATX80
INLINE void set_floatx80_rounding_precision(int val STATUS_PARAM) { STATUS(floatx80_rounding_precision) = val; }
#endif

#if defined(CONFIG_BSD) || \
    (defined(CONFIG_SOLARIS) && CONFIG_SOLARIS_VERSION < 10)
#define lrint(d)		((int32_t)rint(d))
#define llrint(d)		((int64_t)rint(d))
#define lrintf(f)		((int32_t)rint(f))
#define llrintf(f)		((int64_t)rint(f))
#define sqrtf(f)		((float)sqrt(f))
#define remainderf(fa, fb)	((float)remainder(fa, fb))
#define rintf(f)		((float)rint(f))
#if !defined(__sparc__) && \
    (defined(CONFIG_SOLARIS) && CONFIG_SOLARIS_VERSION < 10)
extern long double rintl(long double);
extern long double scalbnl(long double, int);

long long
llrintl(long double x) {
	return ((long long) rintl(x));
}

long
lrintl(long double x) {
	return ((long) rintl(x));
}

long double
ldexpl(long double x, int n) {
	return (scalbnl(x, n));
}
#endif
#endif

#if defined(_ARCH_PPC)

/* correct (but slow) PowerPC rint() (glibc version is incorrect) */
static double qemu_rint(double x)
{
    double y = 4503599627370496.0;
    if (fabs(x) >= y)
        return x;
    if (x < 0)
        y = -y;
    y = (x + y) - y;
    if (y == 0.0)
        y = copysign(y, x);
    return y;
}

#define rint qemu_rint
#endif

/*----------------------------------------------------------------------------
| Software IEC/IEEE integer-to-floating-point conversion routines.
*----------------------------------------------------------------------------*/
INLINE float32 int32_to_float32( int v STATUS_PARAM) { return (float32)v; }
INLINE float32 uint32_to_float32( unsigned int v STATUS_PARAM) { return (float32)v; }
INLINE float64 int32_to_float64( int v STATUS_PARAM) { return (float64)v; }
INLINE float64 uint32_to_float64( unsigned int v STATUS_PARAM) { return (float64)v; }
#ifdef FLOATX80
INLINE floatx80 int32_to_floatx80( int v STATUS_PARAM) { return (floatx80)v; }
#endif
#ifdef FLOAT128
float128 int32_to_float128( int STATUS_PARAM);
#endif
INLINE float32 int64_to_float32( int64_t v STATUS_PARAM) { return (float32)v; }
INLINE float32 uint64_to_float32( uint64_t v STATUS_PARAM) { return (float32)v; }
INLINE float64 int64_to_float64( int64_t v STATUS_PARAM) { return (float64)v; }
INLINE float64 uint64_to_float64( uint64_t v STATUS_PARAM) { return (float64)v; }
#ifdef FLOATX80
INLINE floatx80 int64_to_floatx80( int64_t v STATUS_PARAM) { return (floatx80)v; }
#endif
#ifdef FLOAT128
float128 int64_to_float128( int64_t STATUS_PARAM);
#endif

/*----------------------------------------------------------------------------
| Software IEC/IEEE single-precision conversion routines.
*----------------------------------------------------------------------------*/
INLINE int float32_to_int32( float32 a STATUS_PARAM) { return long_to_int32(lrintf(a)); }
INLINE int float32_to_int32_round_to_zero( float32 a STATUS_PARAM) { return (int)a; }
INLINE unsigned int float32_to_uint32( float32 a STATUS_PARAM)
{
    int64_t v;
    unsigned int res;
    
    v = llrintf(a);
    if (v < 0) {
        res = 0;
    } else if (v > 0xffffffff) {
        res = 0xffffffff;
    } else {
        res = v;
    }
    return res;
}
INLINE unsigned int float32_to_uint32_round_to_zero( float32 a STATUS_PARAM)
{
    int64_t v;
    unsigned int res;
    
    v = (int64_t)a;
    if (v < 0) {
        res = 0;
    } else if (v > 0xffffffff) {
        res = 0xffffffff;
    } else {
        res = v;
    }
    return res;
}
INLINE int64_t float32_to_int64( float32 a STATUS_PARAM) { return llrintf(a); }
INLINE int64_t float32_to_int64_round_to_zero( float32 a STATUS_PARAM) { return (int64_t)a; }
INLINE float64 float32_to_float64( float32 a STATUS_PARAM) { return a; }
#ifdef FLOATX80
INLINE floatx80 float32_to_floatx80( float32 a STATUS_PARAM) { return a; }
#endif
#ifdef FLOAT128
float128 float32_to_float128( float32  STATUS_PARAM);
#endif

/*----------------------------------------------------------------------------
| Software IEC/IEEE single-precision operations.
*----------------------------------------------------------------------------*/
INLINE float32 float32_round_to_int( float32 a STATUS_PARAM) { return rintf(a); }
INLINE float32 float32_add( float32 a, float32 b STATUS_PARAM)
{
    return a + b;
}
INLINE float32 float32_sub( float32 a, float32 b STATUS_PARAM)
{
    return a - b;
}
INLINE float32 float32_mul( float32 a, float32 b STATUS_PARAM)
{
    return a * b;
}
INLINE float32 float32_div( float32 a, float32 b STATUS_PARAM)
{
    return a / b;
}
INLINE float32 float32_rem( float32 a, float32 b STATUS_PARAM) { return remainderf(a, b); }
INLINE float32 float32_sqrt( float32 a STATUS_PARAM) { return sqrtf(a); }
INLINE int float32_eq( float32 a, float32 b STATUS_PARAM)
{
    return a == b;
}
INLINE int float32_le( float32 a, float32 b STATUS_PARAM)
{
    return a <= b;
}
INLINE int float32_lt( float32 a, float32 b STATUS_PARAM)
{
    return a < b;
}
INLINE int float32_eq_signaling( float32 a, float32 b STATUS_PARAM)
{
    return a <= b && a >= b;
}
INLINE int float32_le_quiet( float32 a, float32 b STATUS_PARAM)
{
    return islessequal(a, b);
}
INLINE int float32_lt_quiet( float32 a, float32 b STATUS_PARAM)
{
    return isless(a, b);
}
INLINE int float32_unordered( float32 a, float32 b STATUS_PARAM)
{
    return isunordered(a, b);

}
INLINE int float32_compare( float32 a, float32 b STATUS_PARAM )
{
    if (a < b) {
        return float_relation_less;
    } else if (a == b) {
        return float_relation_equal;
    } else if (a > b) {
        return float_relation_greater;
    } else {
        return float_relation_unordered;
    }
}
INLINE int float32_compare_quiet( float32 a, float32 b STATUS_PARAM )
{
    if (isless(a, b)) {
        return float_relation_less;
    } else if (a == b) {
        return float_relation_equal;
    } else if (isgreater(a, b)) {
        return float_relation_greater;
    } else {
        return float_relation_unordered;
    }
}
INLINE int float32_is_signaling_nan( float32 a1 )
{
    float32u u;
    uint32_t a;
    u.f = a1;
    a = u.i;
    return ( ( ( a>>22 ) & 0x1FF ) == 0x1FE ) && ( a & 0x003FFFFF );
}
INLINE int float32_is_nan( float32 a1 )
{
    float32u u;
    uint64_t a;
    u.f = a1;
    a = u.i;
    return ( 0xFF800000 < ( a<<1 ) );
}

INLINE float32 float32_abs(float32 a)
{
    return fabsf(a);
}

INLINE float32 float32_chs(float32 a)
{
    return -a;
}

INLINE float32 float32_is_infinity(float32 a)
{
    return fpclassify(a) == FP_INFINITE;
}

INLINE float32 float32_is_neg(float32 a)
{
    float32u u;
    u.f = a;
    return u.i >> 31;
}

INLINE float32 float32_is_zero(float32 a)
{
    return fpclassify(a) == FP_ZERO;
}

INLINE float32 float32_scalbn(float32 a, int n)
{
    return scalbnf(a, n);
}

/*----------------------------------------------------------------------------
| Software IEC/IEEE double-precision conversion routines.
*----------------------------------------------------------------------------*/
INLINE int float64_to_int32( float64 a STATUS_PARAM ) { return long_to_int32(lrint(a)); }
INLINE int float64_to_int32_round_to_zero( float64 a STATUS_PARAM ) { return (int)a; }
INLINE unsigned int float64_to_uint32( float64 a STATUS_PARAM )
{
    int64_t v;
    unsigned int res;
    
    v = llrint(a);
    if (v < 0) {
        res = 0;
    } else if (v > 0xffffffff) {
        res = 0xffffffff;
    } else {
        res = v;
    }
    return res;
}
INLINE unsigned int float64_to_uint32_round_to_zero( float64 a STATUS_PARAM )
{
    int64_t v;
    unsigned int res;
    
    v = (int64_t)a;
    if (v < 0) {
        res = 0;
    } else if (v > 0xffffffff) {
        res = 0xffffffff;
    } else {
        res = v;
    }
    return res;
}
INLINE int64_t float64_to_int64( float64 a STATUS_PARAM ) { return llrint(a); }
INLINE int64_t float64_to_int64_round_to_zero( float64 a STATUS_PARAM ) { return (int64_t)a; }
INLINE uint64_t float64_to_uint64( float64 a STATUS_PARAM )
{ 
    int64_t v;
    
    v = llrint(a + (float64)INT64_MIN);
    
    return v - INT64_MIN;
}
INLINE uint64_t float64_to_uint64_round_to_zero( float64 a STATUS_PARAM )
{
    int64_t v;

    v = (int64_t)(a + (float64)INT64_MIN);
    
    return v - INT64_MIN;
}
INLINE float32 float64_to_float32( float64 a STATUS_PARAM ) { return a; }
#ifdef FLOATX80
INLINE floatx80 float64_to_floatx80( float64 a STATUS_PARAM ) { return a; }
#endif
#ifdef FLOAT128
INLINE float128 float64_to_float128( float64 STATUS_PARAM ) { return a; }
#endif

/*----------------------------------------------------------------------------
| Software IEC/IEEE double-precision operations.
*----------------------------------------------------------------------------*/
#if defined(__sun__) && \
    (defined(CONFIG_SOLARIS) && CONFIG_SOLARIS_VERSION < 10)
static inline float64 trunc(float64 x)
{
    return x < 0 ? -floor(-x) : floor(x);
}
#endif
INLINE float64 float64_round_to_int( float64 a STATUS_PARAM ) { return rint(a); }
INLINE float64 float64_trunc_to_int( float64 a STATUS_PARAM ) { return trunc(a); }
INLINE float64 float64_add( float64 a, float64 b STATUS_PARAM)
{
    return a + b;
}
INLINE float64 float64_sub( float64 a, float64 b STATUS_PARAM)
{
    return a - b;
}
INLINE float64 float64_mul( float64 a, float64 b STATUS_PARAM)
{
    return a * b;
}
INLINE float64 float64_div( float64 a, float64 b STATUS_PARAM)
{
    return a / b;
}
INLINE float64 float64_rem( float64 a, float64 b STATUS_PARAM ) { return remainder(a, b); }
INLINE float64 float64_sqrt( float64 a STATUS_PARAM ) { return sqrt(a); }
INLINE int float64_eq( float64 a, float64 b STATUS_PARAM)
{
    return a == b;
}
INLINE int float64_le( float64 a, float64 b STATUS_PARAM)
{
    return a <= b;
}
INLINE int float64_lt( float64 a, float64 b STATUS_PARAM)
{
    return a < b;
}
INLINE int float64_eq_signaling( float64 a, float64 b STATUS_PARAM)
{
    return a <= b && a >= b;
}
INLINE int float64_le_quiet( float64 a, float64 b STATUS_PARAM)
{
    return islessequal(a, b);
}
INLINE int float64_lt_quiet( float64 a, float64 b STATUS_PARAM)
{
    return isless(a, b);

}
INLINE int float64_unordered( float64 a, float64 b STATUS_PARAM)
{
    return isunordered(a, b);

}
INLINE int float64_compare( float64 a, float64 b STATUS_PARAM )
{
    if (a < b) {
        return float_relation_less;
    } else if (a == b) {
        return float_relation_equal;
    } else if (a > b) {
        return float_relation_greater;
    } else {
        return float_relation_unordered;
    }
}
INLINE int float64_compare_quiet( float64 a, float64 b STATUS_PARAM )
{
    if (isless(a, b)) {
        return float_relation_less;
    } else if (a == b) {
        return float_relation_equal;
    } else if (isgreater(a, b)) {
        return float_relation_greater;
    } else {
        return float_relation_unordered;
    }
}
INLINE int float64_is_signaling_nan( float64 a1 )
{
    float64u u;
    uint64_t a;
    u.f = a1;
    a = u.i;
    return
	   ( ( ( a>>51 ) & 0xFFF ) == 0xFFE )
	&& ( a & LIT64( 0x0007FFFFFFFFFFFF ) );
}
INLINE int float64_is_nan( float64 a1 )
{
    float64u u;
    uint64_t a;
    u.f = a1;
    a = u.i;
    
    return ( LIT64( 0xFFF0000000000000 ) < (bits64) ( a<<1 ) );
}

INLINE float64 float64_abs(float64 a)
{
    return fabs(a);
}

INLINE float64 float64_chs(float64 a)
{
    return -a;
}

INLINE float64 float64_is_infinity(float64 a)
{
    return fpclassify(a) == FP_INFINITE;
}

INLINE float64 float64_is_neg(float64 a)
{
    float64u u;
    u.f = a;
    return u.i >> 63;
}

INLINE float64 float64_is_zero(float64 a)
{
    return fpclassify(a) == FP_ZERO;
}

INLINE float64 float64_scalbn(float64 a, int n)
{
    return scalbn(a, n);
}

#ifdef FLOATX80

/*----------------------------------------------------------------------------
| Software IEC/IEEE extended double-precision conversion routines.
*----------------------------------------------------------------------------*/
INLINE int floatx80_to_int32( floatx80 a STATUS_PARAM ) { return long_to_int32(lrintl(a)); }
INLINE int floatx80_to_int32_round_to_zero( floatx80 a STATUS_PARAM ) { return (int)a; }
INLINE int64_t floatx80_to_int64( floatx80 a STATUS_PARAM ) { return llrintl(a); }
INLINE int64_t floatx80_to_int64_round_to_zero( floatx80 a STATUS_PARAM ) { return (int64_t)a; }
INLINE float32 floatx80_to_float32( floatx80 a STATUS_PARAM ) { return a; }
INLINE float64 floatx80_to_float64( floatx80 a STATUS_PARAM ) { return a; }
#ifdef FLOAT128
float128 floatx80_to_float128( floatx80 STATUS_PARAM );
#endif

/*----------------------------------------------------------------------------
| Software IEC/IEEE extended double-precision operations.
*----------------------------------------------------------------------------*/
INLINE floatx80 floatx80_round_to_int( floatx80 a STATUS_PARAM ) { return rintl(a); }
INLINE floatx80 floatx80_add( floatx80 a, floatx80 b STATUS_PARAM)
{
    return a + b;
}
INLINE floatx80 floatx80_sub( floatx80 a, floatx80 b STATUS_PARAM)
{
    return a - b;
}
INLINE floatx80 floatx80_mul( floatx80 a, floatx80 b STATUS_PARAM)
{
    return a * b;
}
INLINE floatx80 floatx80_div( floatx80 a, floatx80 b STATUS_PARAM)
{
    return a / b;
}
INLINE floatx80 floatx80_rem( floatx80 a, floatx80 b STATUS_PARAM ) { return remainderl(a, b); }
INLINE floatx80 floatx80_sqrt( floatx80 a STATUS_PARAM ) { return sqrtl(a); }
INLINE int floatx80_eq( floatx80 a, floatx80 b STATUS_PARAM)
{
    return a == b;
}
INLINE int floatx80_le( floatx80 a, floatx80 b STATUS_PARAM)
{
    return a <= b;
}
INLINE int floatx80_lt( floatx80 a, floatx80 b STATUS_PARAM)
{
    return a < b;
}
INLINE int floatx80_eq_signaling( floatx80 a, floatx80 b STATUS_PARAM)
{
    return a <= b && a >= b;
}
INLINE int floatx80_le_quiet( floatx80 a, floatx80 b STATUS_PARAM)
{
    return islessequal(a, b);
}
INLINE int floatx80_lt_quiet( floatx80 a, floatx80 b STATUS_PARAM)
{
    return isless(a, b);

}
INLINE int floatx80_unordered( floatx80 a, floatx80 b STATUS_PARAM)
{
    return isunordered(a, b);

}
INLINE int floatx80_compare( floatx80 a, floatx80 b STATUS_PARAM )
{
    if (a < b) {
        return float_relation_less;
    } else if (a == b) {
        return float_relation_equal;
    } else if (a > b) {
        return float_relation_greater;
    } else {
        return float_relation_unordered;
    }
}
INLINE int floatx80_compare_quiet( floatx80 a, floatx80 b STATUS_PARAM )
{
    if (isless(a, b)) {
        return float_relation_less;
    } else if (a == b) {
        return float_relation_equal;
    } else if (isgreater(a, b)) {
        return float_relation_greater;
    } else {
        return float_relation_unordered;
    }
}
INLINE int floatx80_is_signaling_nan( floatx80 a1 )
{
    floatx80u u;
    uint64_t aLow;
    u.f = a1;

    aLow = u.i.low & ~ LIT64( 0x4000000000000000 );
    return
           ( ( u.i.high & 0x7FFF ) == 0x7FFF )
        && (bits64) ( aLow<<1 )
        && ( u.i.low == aLow );
}
INLINE int floatx80_is_nan( floatx80 a1 )
{
    floatx80u u;
    u.f = a1;
    return ( ( u.i.high & 0x7FFF ) == 0x7FFF ) && (bits64) ( u.i.low<<1 );
}

INLINE floatx80 floatx80_abs(floatx80 a)
{
    return fabsl(a);
}

INLINE floatx80 floatx80_chs(floatx80 a)
{
    return -a;
}

INLINE floatx80 floatx80_is_infinity(floatx80 a)
{
    return fpclassify(a) == FP_INFINITE;
}

INLINE floatx80 floatx80_is_neg(floatx80 a)
{
    floatx80u u;
    u.f = a;
    return u.i.high >> 15;
}

INLINE floatx80 floatx80_is_zero(floatx80 a)
{
    return fpclassify(a) == FP_ZERO;
}

INLINE floatx80 floatx80_scalbn(floatx80 a, int n)
{
    return scalbnl(a, n);
}

#endif
#endif
