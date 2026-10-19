/**
 * @file ow_compiler.h
 * @brief Private compiler-attribute shim for the driver sources.
 *
 * The DS18B20 driver marks its two optional callbacks weak and forces a handful
 * of small helpers inline. Those spellings (__WEAK, __STATIC_FORCEINLINE) come
 * from CMSIS, but the driver needs no other CMSIS or device fact - it talks to
 * the bus only through the onewire_* API - so rather than drag the whole device
 * header into its translation unit it takes just the attributes from here,
 * with guarded fallbacks that match CMSIS exactly.
 *
 * The #ifndef guards are what make this safe in either inclusion order:
 * CMSIS's cmsis_gcc.h defines the same names under the same guards, so
 * whichever header is seen first wins and the other is a no-op.
 *
 * Library-internal (src/): reached only by the driver sources, never from a
 * public header.
 */

#ifndef OW_COMPILER_H
#define OW_COMPILER_H

#ifndef __STATIC_FORCEINLINE
#define __STATIC_FORCEINLINE static __attribute__((always_inline)) inline
#endif

#ifndef __STATIC_INLINE
#define __STATIC_INLINE static inline
#endif

#ifndef __WEAK
#define __WEAK __attribute__((weak))
#endif

#endif /* OW_COMPILER_H */
