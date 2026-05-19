/*
 * AdventOS compiler-runtime helpers for the tcc cross-build.
 *
 * tcc.c compiled by gcc emits calls into libgcc's compiler-rt for two
 * categories of operation:
 *
 *   1. 64-bit integer arithmetic on i386 (no native 64-bit registers):
 *      __divdi3, __moddi3, __udivdi3, __umoddi3, __ashldi3, __ashrdi3,
 *      __lshrdi3. These are REAL implementations — tcc.c uses 64-bit
 *      ints when parsing large literals and computing struct sizes.
 *
 *   2. Soft-float helpers for `long double` / double / float arith
 *      that the i386 isn't told to emit x87 for in our build: __addxf3,
 *      __mulxf3, conversion + comparison cousins. tcc uses these when
 *      parsing FP-literal source code. AdventOS user code is integer-
 *      only, so these stubs return 0 — wrong float compile-time semantics
 *      but harmless because nothing reads the result.
 *
 * Real implementations of (1) are ported from tcc/lib/libtcc1.c (LGPL
 * 2.1). Stubs in (2) are written here.
 */

typedef unsigned int  u32;
typedef unsigned long long u64;
typedef signed long long   s64;
typedef long double   xfp;

/* ---- 64-bit integer division — adapted from tcc/lib/libtcc1.c ---- */

static u64 udivmoddi4(u64 num, u64 den, u64 *rem_p) {
    u64 quot = 0, qbit = 1;
    if (den == 0) { return 1 / ((unsigned)den); }   /* trigger #DE */
    while ((s64)den >= 0) {
        den <<= 1;
        qbit <<= 1;
    }
    while (qbit) {
        if (den <= num) { num -= den; quot += qbit; }
        den  >>= 1;
        qbit >>= 1;
    }
    if (rem_p) *rem_p = num;
    return quot;
}

s64 __divdi3(s64 a, s64 b) {
    int neg = 0;
    u64 ua = (u64)a, ub = (u64)b;
    if (a < 0) { ua = -ua; neg = !neg; }
    if (b < 0) { ub = -ub; neg = !neg; }
    u64 q = udivmoddi4(ua, ub, 0);
    return neg ? -(s64)q : (s64)q;
}

s64 __moddi3(s64 a, s64 b) {
    int neg = 0;
    u64 ua = (u64)a, ub = (u64)b;
    u64 r;
    if (a < 0) { ua = -ua; neg = 1; }
    if (b < 0) { ub = -ub; }
    udivmoddi4(ua, ub, &r);
    return neg ? -(s64)r : (s64)r;
}

u64 __udivdi3(u64 a, u64 b) { return udivmoddi4(a, b, 0); }
u64 __umoddi3(u64 a, u64 b) { u64 r; udivmoddi4(a, b, &r); return r; }

s64 __ashldi3(s64 a, int b) { return (s64)((u64)a << (b & 63)); }
s64 __ashrdi3(s64 a, int b) {
    int shift = b & 63;
    return (a < 0) ? (s64)(~(~(u64)a >> shift)) : (s64)((u64)a >> shift);
}
u64 __lshrdi3(u64 a, int b) { return a >> (b & 63); }

/* ---- Float-arith stubs — return 0; safe as long as user source is
 *      integer-only. ----- */

xfp __addxf3(xfp a, xfp b) { (void)a; (void)b; return 0; }
xfp __subxf3(xfp a, xfp b) { (void)a; (void)b; return 0; }
xfp __mulxf3(xfp a, xfp b) { (void)a; (void)b; return 0; }
xfp __divxf3(xfp a, xfp b) { (void)a; (void)b; return 0; }
double __divdf3(double a, double b) { (void)a; (void)b; return 0; }
float  __addsf3(float a, float b)   { (void)a; (void)b; return 0; }
float  __subsf3(float a, float b)   { (void)a; (void)b; return 0; }
float  __mulsf3(float a, float b)   { (void)a; (void)b; return 0; }
float  __divsf3(float a, float b)   { (void)a; (void)b; return 0; }
double __adddf3(double a, double b) { (void)a; (void)b; return 0; }
double __subdf3(double a, double b) { (void)a; (void)b; return 0; }
double __muldf3(double a, double b) { (void)a; (void)b; return 0; }

/* comparison */
int __eqxf2(xfp a, xfp b) { (void)a; (void)b; return 0; }
int __nexf2(xfp a, xfp b) { (void)a; (void)b; return 0; }
int __ltxf2(xfp a, xfp b) { (void)a; (void)b; return 0; }
int __lexf2(xfp a, xfp b) { (void)a; (void)b; return 0; }
int __gtxf2(xfp a, xfp b) { (void)a; (void)b; return 0; }
int __gexf2(xfp a, xfp b) { (void)a; (void)b; return 0; }
int __eqsf2(float a, float b)  { (void)a; (void)b; return 0; }
int __nesf2(float a, float b)  { (void)a; (void)b; return 0; }
int __ltsf2(float a, float b)  { (void)a; (void)b; return 0; }
int __lesf2(float a, float b)  { (void)a; (void)b; return 0; }
int __gtsf2(float a, float b)  { (void)a; (void)b; return 0; }
int __gesf2(float a, float b)  { (void)a; (void)b; return 0; }
int __eqdf2(double a, double b){ (void)a; (void)b; return 0; }
int __nedf2(double a, double b){ (void)a; (void)b; return 0; }
int __ltdf2(double a, double b){ (void)a; (void)b; return 0; }
int __ledf2(double a, double b){ (void)a; (void)b; return 0; }
int __gtdf2(double a, double b){ (void)a; (void)b; return 0; }
int __gedf2(double a, double b){ (void)a; (void)b; return 0; }

/* extension / truncation */
xfp     __extenddfxf2(double a)   { (void)a; return 0; }
xfp     __extendsfxf2(float a)    { (void)a; return 0; }
double  __extendsfdf2(float a)    { (void)a; return 0; }
double  __truncxfdf2 (xfp a)      { (void)a; return 0; }
float   __truncxfsf2 (xfp a)      { (void)a; return 0; }
float   __truncdfsf2 (double a)   { (void)a; return 0; }

/* int / float conversions */
s64     __fixxfdi    (xfp a)      { (void)a; return 0; }
u64     __fixunsxfdi (xfp a)      { (void)a; return 0; }
s64     __fixdfdi    (double a)   { (void)a; return 0; }
u64     __fixunsdfdi (double a)   { (void)a; return 0; }
s64     __fixsfdi    (float a)    { (void)a; return 0; }
u64     __fixunssfdi (float a)    { (void)a; return 0; }
int     __fixxfsi    (xfp a)      { (void)a; return 0; }
unsigned __fixunsxfsi(xfp a)      { (void)a; return 0; }
int     __fixdfsi    (double a)   { (void)a; return 0; }
unsigned __fixunsdfsi(double a)   { (void)a; return 0; }
int     __fixsfsi    (float a)    { (void)a; return 0; }
unsigned __fixunssfsi(float a)    { (void)a; return 0; }
float   __floatsisf  (int a)      { (void)a; return 0; }
double  __floatsidf  (int a)      { (void)a; return 0; }
xfp     __floatsixf  (int a)      { (void)a; return 0; }
float   __floatdisf  (s64 a)      { (void)a; return 0; }
double  __floatdidf  (s64 a)      { (void)a; return 0; }
xfp     __floatdixf  (s64 a)      { (void)a; return 0; }
double  __floatunsidf(unsigned a) { (void)a; return 0; }
xfp     __floatunsixf(unsigned a) { (void)a; return 0; }
float   __floatunsisf(unsigned a) { (void)a; return 0; }
float   __floatundisf(u64 a)      { (void)a; return 0; }
double  __floatundidf(u64 a)      { (void)a; return 0; }
xfp     __floatundixf(u64 a)      { (void)a; return 0; }

/* misc */
xfp     __negxf2     (xfp a)      { (void)a; return 0; }
double  __negdf2     (double a)   { (void)a; return 0; }
float   __negsf2     (float a)    { (void)a; return 0; }
