#ifndef SM_TYPES_H_
#define SM_TYPES_H_

#pragma warning(disable: 4244)

#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>

#if defined(_WIN32) && !COMPILER_TCC
#include <crtdbg.h>
#define assert _ASSERTE
#else
#include <assert.h>
#endif


typedef uint8_t uint8;
typedef int8_t int8;
#if defined(__cplusplus) && defined(SM_LE_TYPES)
// The game code sometimes memcpy()s between overlapping RAM regions (e.g. when
// unpacking room data into place). That is undefined behaviour; on PC glibc
// memcpy happens to behave like memmove, which is also what the SNES block move
// does. newlib's memcpy copies forward and corrupts the data, so use memmove.
#include <string.h>
#define memcpy memmove
// ---------------------------------------------------------------------------
// Byte-order-independent integers for big-endian targets (N64).
//
// The game code (written for x86) reads and writes SNES RAM/ROM, and its own
// tables of SNES data, through uint16/uint32 pointers and struct overlays.
// These wrappers always store values in little-endian (SNES) byte order with
// 1-byte alignment, so every such access means exactly what it meant on a PC:
// no byte swapping bugs, no unaligned-access exceptions. They behave like the
// plain integer types in expressions (implicit conversion), assignments,
// compound assignments and ++/--.
// ---------------------------------------------------------------------------
template<typename T, typename U>
struct LeInt {
  uint8_t b[sizeof(T)];
  LeInt() = default;
  constexpr LeInt(T v) : b{} { set(v); }
  template<typename T2, typename U2>
  constexpr LeInt(LeInt<T2, U2> o) : b{} { set((T)(T2)o); }
  constexpr operator T() const {
    U v = 0;
    for (unsigned i = 0; i < sizeof(T); i++) v |= (U)b[i] << (8 * i);
    return (T)v;
  }
  constexpr void set(T v) {
    U u = (U)v;
    for (unsigned i = 0; i < sizeof(T); i++) b[i] = (uint8_t)(u >> (8 * i));
  }
  constexpr LeInt &operator=(T v) { set(v); return *this; }
  template<typename T2, typename U2>
  constexpr LeInt &operator=(LeInt<T2, U2> o) { set((T)(T2)o); return *this; }
  template<typename V> LeInt &operator+=(V v) { set((T)((T)*this + v)); return *this; }
  template<typename V> LeInt &operator-=(V v) { set((T)((T)*this - v)); return *this; }
  template<typename V> LeInt &operator*=(V v) { set((T)((T)*this * v)); return *this; }
  template<typename V> LeInt &operator/=(V v) { set((T)((T)*this / v)); return *this; }
  template<typename V> LeInt &operator%=(V v) { set((T)((T)*this % v)); return *this; }
  template<typename V> LeInt &operator&=(V v) { set((T)((T)*this & v)); return *this; }
  template<typename V> LeInt &operator|=(V v) { set((T)((T)*this | v)); return *this; }
  template<typename V> LeInt &operator^=(V v) { set((T)((T)*this ^ v)); return *this; }
  template<typename V> LeInt &operator<<=(V v) { set((T)((T)*this << v)); return *this; }
  template<typename V> LeInt &operator>>=(V v) { set((T)((T)*this >> v)); return *this; }
  constexpr auto operator+() const { return +(T)*this; }
  constexpr auto operator-() const { return -(T)*this; }
  LeInt &operator++() { set((T)((T)*this + 1)); return *this; }
  LeInt &operator--() { set((T)((T)*this - 1)); return *this; }
  T operator++(int) { T old = *this; set((T)(old + 1)); return old; }
  T operator--(int) { T old = *this; set((T)(old - 1)); return old; }
};
typedef LeInt<uint16_t, uint16_t> uint16;
typedef LeInt<int16_t, uint16_t> int16;
typedef LeInt<uint32_t, uint32_t> uint32;
typedef LeInt<int32_t, uint32_t> int32;
static_assert(sizeof(uint16) == 2 && alignof(uint16) == 1, "LeInt layout");
static_assert(sizeof(uint32) == 4 && alignof(uint32) == 1, "LeInt layout");
#else
typedef uint16_t uint16;
typedef int16_t int16;
typedef uint32_t uint32;
typedef int32_t int32;
#endif
typedef uint64_t uint64;
typedef int64_t int64;
typedef unsigned int uint;

typedef uint16 VoidP;

#define arraysize(x) sizeof(x)/sizeof(x[0])
#define sign8(x) ((x) & 0x80)
#define sign16(x) ((x) & 0x8000)
#define sign32(x) ((x) & 0x80000000)
#define load24(x) ((*(uint32*)&(x))&0xffffff)

#ifdef _MSC_VER
#define countof _countof
#define NORETURN __declspec(noreturn)
#define FORCEINLINE __forceinline
#define NOINLINE __declspec(noinline)
#else
#define countof(a) (sizeof(a)/sizeof(*(a)))
#define NORETURN
#define FORCEINLINE inline
#define NOINLINE
#endif

#ifdef _DEBUG
#define kDebugFlag 1
#else
#define kDebugFlag 0
#endif

static FORCEINLINE uint16 abs16(uint16 t) { return sign16(t) ? -t : +t; }
static FORCEINLINE uint8 abs8(uint8 t) { return sign8(t) ? -t : t; }
static FORCEINLINE int IntMin(int a, int b) { return a < b ? a : b; }
static FORCEINLINE int IntMax(int a, int b) { return a > b ? a : b; }
static FORCEINLINE uint UintMin(uint a, uint b) { return a < b ? a : b; }
static FORCEINLINE uint UintMax(uint a, uint b) { return a > b ? a : b; }

// windows.h defines this too
#ifdef HIBYTE
#undef HIBYTE
#endif

#define BYTE(x) (*(uint8*)&(x))
#define WORD(x) (*(uint16*)&(x))
#define DWORD(x) (*(uint32*)&(x))
#define XY(x, y) ((y)*64+(x))

static inline uint16 swap16(uint16 v) { return (v << 8) | (v >> 8); }

void NORETURN Die(const char *error);
void Warning(const char *error);

// compile time assertion
#define __CASSERT_N0__(l) COMPILE_TIME_ASSERT_ ## l
#define __CASSERT_N1__(l) __CASSERT_N0__(l)
#define CASSERT(cnd) typedef char __CASSERT_N1__(__LINE__) [(cnd) ? 1 : -1]

#define USE_COROUTINES 1

typedef uint8_t CoroutineRet;

#define COROUTINE_BEGIN(state_var, start_pos) \
  uint8 *_state_variable_ = &(state_var);     \
  switch(*_state_variable_) {                 \
  case start_pos:

#define COROUTINE_MANUAL_POS(position)         \
  case (position):

#define COROUTINE_AWAIT(position, function)    \
  case (position):                             \
      { uint8 _coret_ = (function);            \
      if (_coret_) {                           \
        *_state_variable_ = (position);        \
        return _coret_;                        \
      }}

#define COROUTINE_AWAIT_ONLY(function)         \
      { uint8 _coret_ = (function);            \
      if (_coret_)                             \
        return _coret_;                        \
      }

#define COROUTINE_END(position)               \
  }                                           \
  *_state_variable_ = (position);             \
  return 0

#define kCoroutineNone 0

/* 90 */
#pragma pack(push, 1)
typedef struct LongPtr {
  VoidP addr;
  uint8 bank;
} LongPtr;
#pragma pack (pop)

static inline bool __CFADD__uint16(uint16 x, uint16 y) { return (uint16)(x) > (uint16)(x + y); }
static inline bool __CFADD__uint8(uint8 x, uint8 y) { return (uint8)(x) > (uint8)(x + y); }

typedef struct PairU16 {
  uint16 k, j;
} PairU16;

typedef struct Point16U {
  uint16 x, y;
} Point16U;

typedef struct Rect16U {
  uint16 x, y, w, h;
} Rect16U;

// Some convenience macros to make partial accesses nicer
#define LAST_IND(x,part_type)    (sizeof(x)/sizeof(part_type) - 1)
#define HIGH_IND(x,part_type)  LAST_IND(x,part_type)
#define LOW_IND(x,part_type)   0
// first unsigned macros:
#define BYTEn(x, n)   (*((uint8*)&(x)+n))
#define WORDn(x, n)   (*((uint16*)&(x)+n))

#define LOBYTE(x)  BYTEn(x,LOW_IND(x,uint8))
#define LOWORD(x)  WORDn(x,LOW_IND(x,uint16))
#define HIBYTE(x)  BYTEn(x,HIGH_IND(x,uint8))
#define HIWORD(x)  WORDn(x,HIGH_IND(x,uint16))

#define GET_HIBYTE(x) (((x) & 0xff00) >> 8)

#define PAIR16(high, low) ((uint16)((high) << 8) | (uint8)low)

// Generate a pair of operands.
#define __PAIR32__(high, low)   (((uint32) (high) << 16) | (uint16)(low))
#define IPAIR32(high, low)   ((int32)(( (high) << 16) | (uint16)(low)))

// Helper functions to represent some assembly instructions.

// compile time assertion
#define __CASSERT_N0__(l) COMPILE_TIME_ASSERT_ ## l
#define __CASSERT_N1__(l) __CASSERT_N0__(l)
#define CASSERT(cnd) typedef char __CASSERT_N1__(__LINE__) [(cnd) ? 1 : -1]


#endif  // SM_TYPES_H_

