/*
 * The integer<->float conversions ppu-gcc's libgcc for this target does not ship (checked: neither the
 * plain nor a soft-float variant has __floatdisf/__fixsfdi/__fixdfdi), but the pipeline's llc-generated
 * soft-float code calls them with the integer ABI: a double arrives as its 64-bit bit pattern in one
 * GPR, a float in one GPR's low word. The bodies are pure bit arithmetic (the soft double helpers in
 * plat_libm.c are hardware-float-compiled; these must stay integer-ABI, so they cannot call them).
 * Truncation toward zero, like C's cast semantics. Result only tilted where the frame is unrepresentable
 * (the compiler's behaviour in C for such a cast is undefined; saturating is what other libgccs pick).
 */
#include <stdint.h>

static int64_t back_from_bits(uint64_t x) {
    int neg = (int)(x >> 63);
    int e = (int)((x >> 52) & 0x7FF) - 1023;   /* unbiased exponent of a normal number */
    uint64_t frac = (x & 0x000FFFFFFFFFFFFFull) | 0x0010000000000000ull; /* with the hidden bit */
    int64_t r;
    if ((x >> 52 & 0x7FF) == 0x7FF) {          /* inf/nan: saturate */
        return neg ? INT64_MIN : INT64_MAX;
    }
    if (e < 63) {
        if (e < 0) {
            r = 0;                             /* |x| < 1: truncates to 0 */
        } else if (e < 52) {
            r = (int64_t)(frac >> (52 - e));
        } else {
            r = (int64_t)(frac << (e - 52));   /* never overflows: e < 63 keeps 63 bits in 64 */
        }
    } else {
        r = INT64_MAX;                         /* >= 2^63: saturate */
    }
    return neg ? -r : r;
}

/* double -> i64 (the double is the caller's argument bit pattern) */
int64_t __fixdfdi(uint64_t a) {
    return back_from_bits(a);
}

/* llc emits this name for the same thing on this target */
int64_t __floatdisf(uint64_t a) {
    return back_from_bits(a);
}

/* float -> i64 (32-bit pattern, top of the argument word) */
int64_t __fixsfdi(uint32_t a) {
    return back_from_bits((uint64_t)a << 32);
}
