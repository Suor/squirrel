/* see copyright notice in squirrel.h */
#ifndef _SQNAN_H_
#define _SQNAN_H_

#include <limits>

/* The NaN an operation makes from operands that aren't NaN (0/0, inf - inf, sqrt(-1)):
   on x86, the original's CPU, the "default NaN", which is negative; ARM64 makes a
   positive one. Scripts can see the sign (tostring writes "-nan(ind)" or "nan"), so on
   other CPUs such a NaN becomes x86's. A NaN operand passes through as on x86. */
template <typename F>
inline F sq_x86nan(F result, F a, F b)
{
#if defined(__aarch64__) || defined(_M_ARM64)
    if (result != result && a == a && b == b)
        return -std::numeric_limits<F>::quiet_NaN();
#else
    (void)a; (void)b;
#endif
    return result;
}

template <typename F>
inline F sq_x86nan(F result, F a)
{
    return sq_x86nan(result, a, a);
}

#endif /*_SQNAN_H_*/
