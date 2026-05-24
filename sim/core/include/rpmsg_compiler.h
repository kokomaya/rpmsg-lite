/* Override rpmsg_compiler.h with MSVC+GCC support for PC simulation */
#pragma once

#if defined(_MSC_VER)
  #define MEM_BARRIER() _ReadWriteBarrier()
  #ifndef RL_PACKED_BEGIN
  #define RL_PACKED_BEGIN __pragma(pack(push, 1))
  #endif
  #ifndef RL_PACKED_END
  #define RL_PACKED_END   __pragma(pack(pop))
  #endif
#elif defined(__GNUC__)
  #define MEM_BARRIER() __asm__ volatile("" : : : "memory")
  #ifndef RL_PACKED_BEGIN
  #define RL_PACKED_BEGIN
  #endif
  #ifndef RL_PACKED_END
  #define RL_PACKED_END __attribute__((__packed__))
  #endif
#else
  #error "Please add MEM_BARRIER and pack macros for your compiler."
#endif
