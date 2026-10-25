# Compiler settings shared by the FFmpeg and plugin builds (sourced).
# shellcheck disable=SC2034

CROSS=arm-linux-gnueabi-

# ARMv7 with NEON. softfp keeps armel's soft-float calling convention, so the
# result links against the TV's libraries while still using the FPU.
ARCH_CFLAGS="-march=armv7-a -mfpu=neon -mfloat-abi=softfp"

# Hide everything by default, let --gc-sections drop unused code, and keep
# build paths out of the binary.
COMMON_CFLAGS="-O2 -fPIC -fvisibility=hidden -ffunction-sections -fdata-sections -ffile-prefix-map=/build/="
