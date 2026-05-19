/* AdventOS config.h for vendored tcc (session 129).
 *
 * Replaces the autoconf-generated config.h that ships in
 * tcc/config.h after running ./configure. We hardcode the values
 * for i386 / Linux because:
 *   - we don't want autotools as a build dep
 *   - we only ever target i386 ELF (no other arches)
 *   - the host compiler details (GCC_MAJOR/MINOR) only affect a
 *     handful of compatibility shims that don't fire for our build
 */
#define TCC_VERSION "0.9.28rc-adventos"

#define CC_gcc 1
#define CC_NAME CC_gcc
#define GCC_MAJOR 4
#define GCC_MINOR 0

#if !(TCC_TARGET_I386 || TCC_TARGET_X86_64 || TCC_TARGET_ARM || \
      TCC_TARGET_ARM64 || TCC_TARGET_RISCV64 || TCC_TARGET_C67)
#define TCC_TARGET_I386 1
#define CONFIG_TRIPLET "i386-linux-gnu"
#endif

#ifndef CONFIG_TCCDIR
#define CONFIG_TCCDIR "/usr/local/lib/tcc"
#endif

/* Embed tccdefs.h as a C string at compile time (via the c2str
 * helper in build.sh). Disabling this means tcc would need to
 * read tccdefs.h from a runtime path, which won't exist on
 * AdventOS. */
#define CONFIG_TCC_PREDEFS 1
