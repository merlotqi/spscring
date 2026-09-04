#pragma once

// Compiler detection.
#if defined(__clang__)
#define SPSCRING_COMPILER_CLANG 1
#elif defined(__GNU__) || defined(__GNUC__)
#define SPSCRING_COMPILER_GCC 1
#elif defined(_MSC_VER)
#define SPSCRING_COMPILER_MSVC 1
#endif

// Architecture detection (only what the CPU pause primitive needs).
#if defined(__x86_64__) || defined(_M_X64) || defined(__amd64__)
#define SPSCRING_ARCH_X86_64 1
#elif defined(__aarch64__) || defined(_M_ARM64) || defined(__ARM_ARCH_8A)
#define SPSCRING_ARCH_ARM64 1
#elif defined(__i386__) || defined(_M_IX86)
#define SPSCRING_ARCH_X86 1
#elif defined(__arm__) || defined(_M_ARM)
#define SPSCRING_ARCH_ARM 1
#endif

// CPU pause / yield hint used by spin loops (exponential backoff, polling wait).
#if defined(SPSCRING_ARCH_X86_64) || defined(SPSCRING_ARCH_X86)
#if defined(_MSC_VER)
#include <intrin.h>
#define SPSCRING_CPU_PAUSE() _mm_pause()
#else
#define SPSCRING_CPU_PAUSE() __builtin_ia32_pause()
#endif
#elif defined(SPSCRING_ARCH_ARM64) || defined(SPSCRING_ARCH_ARM)
#if defined(_MSC_VER)
#include <intrin.h>
#define SPSCRING_CPU_PAUSE() __yield()
#else
#include <arm_acle.h>
#define SPSCRING_CPU_PAUSE() __yield()
#endif
#else
#include <thread>
#define SPSCRING_CPU_PAUSE() std::this_thread::yield()
#endif

#ifndef SPSCRING_CACHE_LINE_SIZE
#define SPSCRING_CACHE_LINE_SIZE 64
#endif

#define SPSCRING_ALIGNAS_CACHE_LINE alignas(SPSCRING_CACHE_LINE_SIZE)
