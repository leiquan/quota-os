/*
 * Compile the upstream flag rules from WebKit
 * 4d967f4ca7bdd75319e732247e714c2bc634555e (PlatformEnable.h).
 * ENABLE_JIT=0 and ENABLE_C_LOOP=1. That pair does not force the
 * assembler, so the executable pool in ExecutableAllocator.cpp
 * (compiled only under ENABLE(JIT)) is not part of this build.
 */
#define WTF_PLATFORM_GUARD_AGAINST_INDIRECT_INCLUSION
#define ENABLE_JIT 0
#define ENABLE_C_LOOP 1
#define ENABLE_YARR_JIT 0
#define CPU(x) 0
#define PLATFORM(x) 0
#define OS(x) 0
#define USE(x) 0
#define HAVE(x) 0
#define COMPILER(x) 0
#define ENABLE_ASSERTS 0
#define ASSERT_ENABLED 0
#define ASAN_ENABLED 0

#include "wtf/PlatformEnable.h"

#if ENABLE(JIT)
#error "JIT stayed on"
#endif
#if !ENABLE(C_LOOP)
#error "CLoop stayed off"
#endif
#if ENABLE(YARR_JIT)
#error "Yarr JIT stayed on"
#endif
#if ENABLE(JIT) || ENABLE(YARR_JIT) || !ENABLE(C_LOOP)
#error "this flag set still forces ENABLE_ASSEMBLER"
#endif

int quotaos_jsc_interpreter_only_ok = 1;
