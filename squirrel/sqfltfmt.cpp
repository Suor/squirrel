/*
    see copyright notice in squirrel.h
*/
/*
    printf's %e/%E/%f/%g/%G, written the way Battle Brothers' engine (Squirrel 3.0 on
    the MSVC UCRT, with its default "legacy" float rounding) writes them, on every
    platform and independently of the C library and locale:
    - the digits are the value's exact decimal expansion, at any precision;
    - the cut rounds half away from zero (the digit after the cut >= 5 rounds the
      magnitude up), so 2.5 is "3" and 0.125 at %.2f is "0.13";
    - %g picks %e or %f from the exponent after rounding to its precision;
    - exponents have at least two digits ("1e+06");
    - inf is "inf", NaN "nan", the x86 default NaN (negative, quiet, no payload)
      "-nan(ind)", a signalling NaN "nan(snan)" (upper case for %E/%G); the
      precision doesn't apply to them, but width and the 0 flag do ("-000nan(ind)").
    Verified against the original game.

    sq_formatfloat_ieee is the modern variant (format3_2): the same exact digits, but
    ties round to even, as C and IEEE 754 specify, and inf/NaN as glibc writes them
    ("inf", "nan", "-nan"; the 0 flag pads them with spaces).
*/
#include "sqpcheader.h"
#include <string.h>

namespace {

/* Exact decimal digits of a finite double: value = 0.D[0]D[1]...D[len-1] x 10^point,
   D without leading or trailing zeros (len 0 for zero). */
struct Decimal {
    /* A double's exact expansion has at most 309 integer and 1074 fraction digits. */
    char d[1400];
    int len;
    int point;
};

/* Little-endian base-2^32 natural number, big enough for (2^53 - 1) * 5^1074. */
struct BigNat {
    unsigned int limb[84];
    int n;

    void set(unsigned long long v) {
        n = 0;
        while (v) { limb[n++] = (unsigned int)v; v >>= 32; }
    }
    void mul(unsigned int m) {
        unsigned long long carry = 0;
        for (int i = 0; i < n; i++) {
            unsigned long long t = (unsigned long long)limb[i] * m + carry;
            limb[i] = (unsigned int)t;
            carry = t >> 32;
        }
        if (carry) limb[n++] = (unsigned int)carry;
    }
    void shl(int bits) {
        int words = bits / 32, rest = bits % 32;
        if (n == 0) return;
        if (rest) {
            unsigned int carry = 0;
            for (int i = 0; i < n; i++) {
                unsigned int t = limb[i];
                limb[i] = (t << rest) | carry;
                carry = t >> (32 - rest);
            }
            if (carry) limb[n++] = carry;
        }
        if (words) {
            for (int i = n - 1; i >= 0; i--) limb[i + words] = limb[i];
            for (int i = 0; i < words; i++) limb[i] = 0;
            n += words;
        }
    }
    /* Divides by 10^9 in place, returning the remainder. */
    unsigned int divsmall() {
        unsigned long long rem = 0;
        for (int i = n - 1; i >= 0; i--) {
            unsigned long long t = (rem << 32) | limb[i];
            limb[i] = (unsigned int)(t / 1000000000u);
            rem = t % 1000000000u;
        }
        while (n > 0 && limb[n - 1] == 0) n--;
        return (unsigned int)rem;
    }
    /* Writes the decimal digits (most significant first, no leading zeros) to out and
       returns how many. */
    int digits(char *out) {
        char tmp[800];
        int k = 0;
        while (n > 0) {
            unsigned int chunk = divsmall();
            for (int i = 0; i < 9; i++) { tmp[k++] = (char)('0' + chunk % 10); chunk /= 10; }
        }
        while (k > 0 && tmp[k - 1] == '0') k--;
        for (int i = 0; i < k; i++) out[i] = tmp[k - 1 - i];
        return k;
    }
};

void expand(unsigned long long mant, int exp2, Decimal &dec)
{
    /* value = mant * 2^exp2, mant != 0 */
    while (exp2 < 0 && !(mant & 1)) { mant >>= 1; exp2++; }
    BigNat big;
    int len = 0, point = 0;
    if (exp2 >= 0) {
        big.set(mant);
        big.shl(exp2);
        len = big.digits(dec.d);
        point = len;
    }
    else {
        int frac = -exp2;
        unsigned long long ipart = frac < 64 ? mant >> frac : 0;
        unsigned long long fpart = frac < 64 ? mant & ((1ull << frac) - 1) : mant;
        if (ipart) {
            big.set(ipart);
            len = big.digits(dec.d);
        }
        point = len;
        /* fpart / 2^frac = fpart * 5^frac / 10^frac: exactly frac digits */
        big.set(fpart);
        int f = frac;
        while (f >= 13) { big.mul(1220703125u); f -= 13; }  /* 5^13 */
        unsigned int p5 = 1;
        while (f-- > 0) p5 *= 5;
        big.mul(p5);
        char fd[1100];
        int flen = big.digits(fd);
        for (int i = flen; i < frac; i++) dec.d[len++] = '0';  /* leading zeros */
        memcpy(dec.d + len, fd, flen);
        len += flen;
    }
    /* strip leading zeros (moving the point) and trailing zeros */
    int lead = 0;
    while (lead < len && dec.d[lead] == '0') lead++;
    if (lead) { memmove(dec.d, dec.d + lead, len - lead); len -= lead; point -= lead; }
    while (len > 0 && dec.d[len - 1] == '0') len--;
    dec.len = len;
    dec.point = point;
}

/* Keeps the first `keep` digits, rounding the exact digits half away from zero (the
   original's CRT) or half to even. */
void round_to(Decimal &dec, int keep, bool even)
{
    if (keep >= dec.len) return;
    if (keep < 0) { dec.len = 0; return; }
    bool up = dec.d[keep] >= '5';
    /* a tie: the 5 is the last digit (trailing zeros are stripped) */
    if (even && dec.d[keep] == '5' && keep == dec.len - 1)
        up = keep > 0 && ((dec.d[keep - 1] - '0') & 1);
    dec.len = keep;
    if (up) {
        int i = keep - 1;
        while (i >= 0 && dec.d[i] == '9') i--;
        if (i < 0) { dec.d[0] = '1'; dec.len = 1; dec.point++; }
        else { dec.d[i]++; dec.len = i + 1; }
    }
    while (dec.len > 0 && dec.d[dec.len - 1] == '0') dec.len--;
}

struct Out {
    char *p;
    int n;
    void put(char c) { p[n++] = c; }
    void puts(const char *s) { while (*s) put(*s++); }
};

char digit_at(const Decimal &dec, int i) { return i >= 0 && i < dec.len ? dec.d[i] : '0'; }

/* %f body of a rounded decimal */
void put_fixed(Out &o, const Decimal &dec, int prec, bool alt)
{
    if (dec.len == 0 || dec.point <= 0) o.put('0');
    else for (int i = 0; i < dec.point; i++) o.put(digit_at(dec, i));
    if (prec > 0 || alt) o.put('.');
    for (int i = 0; i < prec; i++) o.put(digit_at(dec, dec.point + i));
}

/* %e body of a rounded decimal (exp is the decimal exponent of its first digit) */
void put_exp(Out &o, const Decimal &dec, int prec, bool alt, int exp, char e)
{
    o.put(digit_at(dec, 0));
    if (prec > 0 || alt) o.put('.');
    for (int i = 1; i <= prec; i++) o.put(digit_at(dec, i));
    o.put(e);
    o.put(exp < 0 ? '-' : '+');
    if (exp < 0) exp = -exp;
    char tmp[8];
    int k = 0;
    do { tmp[k++] = (char)('0' + exp % 10); exp /= 10; } while (exp);
    if (k < 2) tmp[k++] = '0';
    while (k) o.put(tmp[--k]);
}

/* Drops %g's trailing fraction zeros (and a bare point) from the body at o.p[start..]. */
void strip_g(Out &o, int start)
{
    int dot = -1, e = o.n;
    for (int i = start; i < o.n; i++) {
        if (o.p[i] == '.') dot = i;
        if (o.p[i] == 'e' || o.p[i] == 'E') { e = i; break; }
    }
    if (dot < 0) return;
    int end = e;
    while (end > dot + 1 && o.p[end - 1] == '0') end--;
    if (end == dot + 1) end = dot;
    int tail = o.n - e;
    memmove(o.p + end, o.p + e, tail);
    o.n = end + tail;
}

}

static SQInteger formatfloat(SQChar *buf, SQInteger bufsize, const SQChar *spec, SQFloat value, bool msvc)
{
    /* parse %[-+ #0]*[width][.prec](e|E|f|g|G) */
    const SQChar *s = spec;
    if (*s++ != '%') return -1;
    bool left = false, plus = false, space = false, alt = false, zero = false;
    for (;; s++) {
        if (*s == '-') left = true;
        else if (*s == '+') plus = true;
        else if (*s == ' ') space = true;
        else if (*s == '#') alt = true;
        else if (*s == '0') zero = true;
        else break;
    }
    int width = 0, prec = -1;
    while (*s >= '0' && *s <= '9') { width = width * 10 + (*s - '0'); if (width > 100000) return -1; s++; }
    if (*s == '.') {
        s++;
        prec = 0;
        while (*s >= '0' && *s <= '9') { prec = prec * 10 + (*s - '0'); if (prec > 100000) return -1; s++; }
    }
    SQChar conv = *s++;
    if (*s != '\0') return -1;
    if (conv != 'e' && conv != 'E' && conv != 'f' && conv != 'g' && conv != 'G') return -1;
    bool upper = conv == 'E' || conv == 'G';

    double v = (double)value;
    unsigned long long bits;
    memcpy(&bits, &v, sizeof(bits));
    bool neg = (bits >> 63) != 0;
    int bexp = (int)((bits >> 52) & 0x7ff);
    unsigned long long frac = bits & 0xfffffffffffffull;

    /* body: everything after the sign; long enough for 309 integer digits plus
       the precision */
    int cap = 400 + (prec > 0 ? prec : 0);
    char stackbody[512];
    char *body = cap <= (int)sizeof(stackbody) ? stackbody : (char *)sq_malloc(cap);
    Out o = { body, 0 };

    if (bexp == 0x7ff) {
        const char *word;
        if (frac == 0) word = upper ? "INF" : "inf";
        else if (!msvc) word = upper ? "NAN" : "nan";
        else if (neg && frac == 0x8000000000000ull) word = upper ? "NAN(IND)" : "nan(ind)";
        else if (!(frac & 0x8000000000000ull)) word = upper ? "NAN(SNAN)" : "nan(snan)";
        else word = upper ? "NAN" : "nan";
        o.puts(word);
    }
    else {
        Decimal dec;
        if (bexp == 0 && frac == 0) { dec.len = 0; dec.point = 0; }
        else if (bexp == 0) expand(frac, -1074, dec);
        else expand(frac | (1ull << 52), bexp - 1075, dec);

        if (conv == 'f') {
            if (prec < 0) prec = 6;
            round_to(dec, dec.point + prec, !msvc);
            put_fixed(o, dec, prec, alt);
        }
        else if (conv == 'e' || conv == 'E') {
            if (prec < 0) prec = 6;
            round_to(dec, prec + 1, !msvc);
            put_exp(o, dec, prec, alt, dec.len ? dec.point - 1 : 0, conv);
        }
        else {
            int p = prec < 0 ? 6 : prec == 0 ? 1 : prec;
            round_to(dec, p, !msvc);
            int x = dec.len ? dec.point - 1 : 0;
            if (p > x && x >= -4) put_fixed(o, dec, p - 1 - x, alt);
            else put_exp(o, dec, p - 1, alt, x, upper ? 'E' : 'e');
            if (!alt) strip_g(o, 0);
        }
    }

    char sign = neg ? '-' : plus ? '+' : space ? ' ' : 0;
    if (!msvc && bexp == 0x7ff) zero = false;
    int len = o.n + (sign ? 1 : 0);
    int pad = width > len ? width - len : 0;
    SQInteger total = len + pad;
    SQInteger w = 0;
#define EMIT(c) do { if (w < bufsize - 1) buf[w] = (SQChar)(c); w++; } while (0)
    if (!left && !zero) for (int i = 0; i < pad; i++) EMIT(' ');
    if (sign) EMIT(sign);
    if (!left && zero) for (int i = 0; i < pad; i++) EMIT('0');
    for (int i = 0; i < o.n; i++) EMIT(body[i]);
    if (left) for (int i = 0; i < pad; i++) EMIT(' ');
#undef EMIT
    if (bufsize > 0) buf[w < bufsize ? w : bufsize - 1] = '\0';
    if (body != stackbody) sq_free(body, cap);
    return total;
}

SQInteger sq_formatfloat(SQChar *buf, SQInteger bufsize, const SQChar *spec, SQFloat value)
{
    return formatfloat(buf, bufsize, spec, value, true);
}

SQInteger sq_formatfloat_ieee(SQChar *buf, SQInteger bufsize, const SQChar *spec, SQFloat value)
{
    return formatfloat(buf, bufsize, spec, value, false);
}
