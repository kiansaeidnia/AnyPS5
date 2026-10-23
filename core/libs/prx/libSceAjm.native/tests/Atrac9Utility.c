#include "utility.h"
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(int) * CHAR_BIT == 32, "requires 32-bit int");
_Static_assert(sizeof(unsigned int) * CHAR_BIT == 32, "requires 32-bit unsigned int");

int main(void)
{
    static const uint32_t values[] = {0u, 1u, 0x7fu, 0x80u, 0x01234567u, 0x80000000u, 0x7fffffffu, 0xffffffffu};
    unsigned checks = 0;
    for (int bits = 0; bits <= 32; ++bits)
    {
        for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
        {
            uint32_t expected = 0;
            for (int bit = 0; bit < bits; ++bit) expected = (expected << 1) | ((values[i] >> bit) & 1u);
            const unsigned int actual = BitReverse32(values[i], bits);
            if (actual != expected)
            {
                fprintf(stderr, "BitReverse32(0x%08x, %d): 0x%08x != 0x%08x\n", (unsigned)values[i], bits, actual, (unsigned)expected);
                return EXIT_FAILURE;
            }
            ++checks;
        }
    }
    for (int bits = 1; bits <= 32; ++bits)
    {
        const uint32_t mask = bits == 32 ? UINT32_MAX : (UINT32_C(1) << bits) - 1u;
        for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
        {
            int value;
            memcpy(&value, &values[i], sizeof(value));
            const uint32_t truncated = values[i] & mask;
            int64_t expected = truncated;
            if (truncated & (UINT32_C(1) << (bits - 1))) expected -= INT64_C(1) << bits;
            const int actual = SignExtend32(value, bits);
            if (actual != expected)
            {
                fprintf(stderr, "SignExtend32(0x%08x, %d): %d != %lld\n", (unsigned)values[i], bits, actual, (long long)expected);
                return EXIT_FAILURE;
            }
            ++checks;
        }
    }
    printf("%u utility checks passed\n", checks);
    return EXIT_SUCCESS;
}
