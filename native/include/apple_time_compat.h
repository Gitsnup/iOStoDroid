#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Narrow, ABI-shaped replacements for four Darwin time APIs. These declarations
 * intentionally avoid importing Apple headers so the same implementation can be
 * built with the Android NDK and exercised by host tests.
 */
typedef struct iostodroid_mach_timebase_info_data {
    uint32_t numer;
    uint32_t denom;
} iostodroid_mach_timebase_info_data_t;

double CFAbsoluteTimeGetCurrent(void);
double CACurrentMediaTime(void);
uint64_t mach_absolute_time(void);
int32_t mach_timebase_info(iostodroid_mach_timebase_info_data_t *info);

#ifdef __cplusplus
}
#endif
