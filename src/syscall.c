/**
 * @file syscall.c
 * @brief Weak newlib-nano syscall retarget stubs for firmware links
 *
 * Makes the small-retarget symbols newlib-nano may pull in resolve without a
 * full syscall layer. Each stub is weak, so a consumer that provides its own
 * (strong) retarget simply overrides it. The CMake build already links
 * --specs=nosys.specs (which ships the same weak stubs), so this TU is the
 * fallback for the Makefile targets and platform consumers; duplicate weak
 * definitions are resolved to one by the linker.
 */
#if defined(__GNUC__) && !defined(__clang__)

__attribute__((weak)) int _close(int file) {
    (void)file;
    return -1;
}

__attribute__((weak)) int _lseek(int file, int ptr, int dir) {
    (void)file;
    (void)ptr;
    (void)dir;
    return -1;
}

__attribute__((weak)) int _read(int file, char *ptr, int len) {
    (void)file;
    (void)ptr;
    (void)len;
    return -1;
}

__attribute__((weak)) int _write(int file, char *ptr, int len) {
    (void)file;
    (void)ptr;
    (void)len;
    return -1;
}

#endif