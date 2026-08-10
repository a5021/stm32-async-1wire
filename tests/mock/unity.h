#ifndef UNITY_SHIM_H
#define UNITY_SHIM_H
/* Minimal Unity-compatible shim: no external dependency. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern int unity_failures;

#define TEST_ASSERT_TRUE(c)                                     \
    do {                                                        \
        if (!(c)) {                                             \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); \
            unity_failures++;                                   \
        }                                                       \
    } while (0)
#define TEST_ASSERT_FALSE(c) TEST_ASSERT_TRUE(!(c))
#define TEST_ASSERT_NOT_EQUAL(a, b) TEST_ASSERT_TRUE((a) != (b))
#define TEST_ASSERT_BITS_HIGH(bits, actual) TEST_ASSERT_TRUE(((actual) & (bits)) == (bits))
#define TEST_ASSERT_BITS_LOW(bits, actual) TEST_ASSERT_TRUE(((actual) & (bits)) == 0)
#define TEST_ASSERT_EQUAL_UINT8(a, b) TEST_ASSERT_TRUE((uint8_t)(a) == (uint8_t)(b))
#define TEST_ASSERT_EQUAL_UINT16(a, b) TEST_ASSERT_TRUE((uint16_t)(a) == (uint16_t)(b))
#define TEST_ASSERT_EQUAL_UINT32(a, b) TEST_ASSERT_TRUE((uint32_t)(a) == (uint32_t)(b))
#define TEST_ASSERT_EQUAL_INT(a, b) TEST_ASSERT_TRUE((int)(a) == (int)(b))
#define TEST_ASSERT_EQUAL_HEX8(a, b) TEST_ASSERT_TRUE((uint8_t)(a) == (uint8_t)(b))
#define TEST_ASSERT_EQUAL_HEX16(a, b) TEST_ASSERT_TRUE((uint16_t)(a) == (uint16_t)(b))
#define TEST_ASSERT_EQUAL_INT16(a, b) TEST_ASSERT_TRUE((int16_t)(a) == (int16_t)(b))
#define TEST_ASSERT_EQUAL_STRING(a, b) \
    TEST_ASSERT_TRUE((a) != 0 && (b) != 0 && strcmp((a), (b)) == 0)
#define TEST_RUN(t)               \
    do {                          \
        setUp();                  \
        printf("  RUN %s\n", #t); \
        t();                      \
    } while (0)

void setUp(void);
void tearDown(void);
#endif /* UNITY_SHIM_H */
