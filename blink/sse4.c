/*-*- mode:c;indent-tabs-mode:nil;c-basic-offset:2;tab-width:8;coding:utf-8 -*-│
│ vi: set et ft=c ts=2 sts=2 sw=2 fenc=utf-8                               :vi │
╞══════════════════════════════════════════════════════════════════════════════╡
│ Permission to use, copy, modify, and/or distribute this software for         │
│ any purpose with or without fee is hereby granted, provided that the         │
│ above copyright notice and this permission notice appear in all copies.      │
│                                                                              │
│ THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL                │
│ WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED                │
│ WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE             │
│ AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL         │
│ DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR        │
│ PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER               │
│ TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR             │
│ PERFORMANCE OF THIS SOFTWARE.                                                │
╚─────────────────────────────────────────────────────────────────────────────*/
// SSE4.1 and SSE4.2: the 66-prefixed ops of the 0F 38 and 0F 3A maps that
// the rest of blink does not run (pmulld and movntdqa it always did, in
// sse.c and ssemov.c, and GetOp routes every other opcode of the two maps
// here). Programs built for x86-64-v2 (Bun, and the binaries it compiles)
// use them without asking CPUID first.
//
// Each map is a table of descriptors: an op names the function that runs it,
// its element size, and the parameters of its family alone, so a family is
// one body and a section here, and an opcode with no entry is #UD. A family
// whose loop is over elements has one function per element size, made from
// its body by SIZED, so the size is a constant the compiler unrolls the lanes
// by rather than a switch on every element. Every family reads its source
// into a local before it writes, so a source that is also the destination
// reads as it was.
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "blink/endian.h"
#include "blink/flags.h"
#include "blink/fpu.h"
#include "blink/machine.h"
#include "blink/modrm.h"
#include "blink/pun.h"
#include "blink/rde.h"

enum Lane { kEq, kGt, kMin, kMax };

struct Sse4Op {
  void (*run)(P, const struct Sse4Op *);
  u8 size;  // element bytes, where the function is not sized; 0 is 4, or 8
            // under REX.W
  union {
    struct {
      enum Lane op;
      bool sign;
    } lanes;
    struct {
      bool sign;
    } widen;
    struct {
      bool scalar;  // the low element alone
    } round;
    struct {
      bool lengths;  // in eax and edx (pcmpestr*), not up to a zero
      bool index;    // in ecx (pcmp*stri), not a mask in xmm0
    } strings;
  };
};

static int ElementSize(const struct Sse4Op *op, u64 rde) {
  return op->size ? op->size : Rexw(rde) ? 8 : 4;
}

forceinline u64 LoadElement(const u8 *p, int size, bool sign) {
  switch (size) {
    case 1:
      return sign ? (u64)(i8)p[0] : p[0];
    case 2:
      return sign ? (u64)(i16)Get16(p) : Get16(p);
    case 4:
      return sign ? (u64)(i32)Get32(p) : Get32(p);
    default:
      return Get64(p);
  }
}

forceinline void StoreElement(u8 *p, int size, u64 x) {
  switch (size) {
    case 1:
      p[0] = x;
      break;
    case 2:
      Put16(p, x);
      break;
    case 4:
      Put32(p, x);
      break;
    default:
      Put64(p, x);
      break;
  }
}

// The r/m operand's n bytes, from an xmm register or memory; the rest zero.
static void Source(P, u8 s[16], size_t n) {
  memcpy(s, GetModrmRegisterXmmPointerRead(A, n), n);
  memset(s + n, 0, 16 - n);
}

// family##n: the family's body with its element size the constant n.
#define SIZED(family, n)                           \
  static void family##n(P, const struct Sse4Op *op) { \
    family##Sized(A, op, n);                       \
  }

static void Dispatch(P, const struct Sse4Op ops[256]) {
  const struct Sse4Op *op = &ops[Opcode(rde)];
  if (!op->run || !Osz(rde)) OpUdImpl(m);
  op->run(A, op);
}

////////////////////////////////////////////////////////////////////////////////
// Blends: pblendvb, blendvps and blendvpd take the source's element where
// xmm0's has its sign bit set; pblendw, blendps and blendpd where imm8 has
// the element's bit set.

forceinline void BlendvSized(P, const struct Sse4Op *op, int size) {
  u8 s[16], *d = XmmRexrReg(m, rde), *mask = m->xmm[0];
  Source(A, s, 16);
  for (int i = 0; i < 16; i += size) {
    if (mask[i + size - 1] & 0x80) memcpy(d + i, s + i, size);
  }
}
SIZED(Blendv, 1)
SIZED(Blendv, 4)
SIZED(Blendv, 8)

forceinline void BlendSized(P, const struct Sse4Op *op, int size) {
  u8 s[16], *d = XmmRexrReg(m, rde);
  Source(A, s, 16);
  for (int i = 0; i < 16 / size; ++i) {
    if (uimm0 & (1 << i)) memcpy(d + i * size, s + i * size, size);
  }
}
SIZED(Blend, 2)
SIZED(Blend, 4)
SIZED(Blend, 8)

////////////////////////////////////////////////////////////////////////////////
// Lane-wise integers: pcmpeqq, pcmpgtq, pminsb and its kin, pmaxsb and its
// kin; pmuldq, packusdw, phminposuw; pmovsx and pmovzx, whose memory source
// is only as many bytes as are widened.

forceinline void LanewiseSized(P, const struct Sse4Op *op, int size) {
  u8 s[16], *d = XmmRexrReg(m, rde);
  bool sign = op->lanes.sign;
  Source(A, s, 16);
  for (int i = 0; i < 16; i += size) {
    u64 x = LoadElement(d + i, size, sign), y = LoadElement(s + i, size, sign);
    bool lt = sign ? (i64)x < (i64)y : x < y;
    u64 r;
    switch (op->lanes.op) {
      case kEq:
        r = x == y ? -1 : 0;
        break;
      case kGt:
        r = !lt && x != y ? -1 : 0;
        break;
      case kMin:
        r = lt ? x : y;
        break;
      default:
        r = lt ? y : x;
        break;
    }
    StoreElement(d + i, size, r);
  }
}
SIZED(Lanewise, 1)
SIZED(Lanewise, 2)
SIZED(Lanewise, 4)
SIZED(Lanewise, 8)

static void Pmuldq(P, const struct Sse4Op *op) {
  u8 s[16], *d = XmmRexrReg(m, rde);
  Source(A, s, 16);
  for (int i = 0; i < 16; i += 8) {
    Put64(d + i, (i64)(i32)Get32(d + i) * (i32)Get32(s + i));
  }
}

static void Packusdw(P, const struct Sse4Op *op) {
  u8 s[16], r[16], *d = XmmRexrReg(m, rde);
  Source(A, s, 16);
  for (int i = 0; i < 8; ++i) {
    i32 x = Get32(i < 4 ? d + i * 4 : s + (i - 4) * 4);
    Put16(r + i * 2, x < 0 ? 0 : x > 0xffff ? 0xffff : x);
  }
  memcpy(d, r, 16);
}

static void Phminposuw(P, const struct Sse4Op *op) {
  u8 s[16], *d = XmmRexrReg(m, rde);
  int min = 0;
  Source(A, s, 16);
  for (int i = 1; i < 8; ++i) {
    if (Get16(s + i * 2) < Get16(s + min * 2)) min = i;
  }
  memset(d, 0, 16);
  Put16(d, Get16(s + min * 2));
  Put16(d + 2, min);
}

// Widen##size##from: [from]-byte elements of the source widened to [size].
forceinline void WidenSized(P, const struct Sse4Op *op, int size, int from) {
  u8 s[16], *d = XmmRexrReg(m, rde);
  int n = 16 / size;
  Source(A, s, n * from);
  for (int i = 0; i < n; ++i) {
    StoreElement(d + i * size, size, LoadElement(s + i * from, from, op->widen.sign));
  }
}
#define WIDEN(size, from)                                 \
  static void Widen##size##from(P, const struct Sse4Op *op) { \
    WidenSized(A, op, size, from);                        \
  }
WIDEN(2, 1)
WIDEN(4, 1)
WIDEN(8, 1)
WIDEN(4, 2)
WIDEN(8, 2)
WIDEN(8, 4)

static void Ptest(P, const struct Sse4Op *op) {
  u8 s[16], *d = XmmRexrReg(m, rde);
  bool zf = true, cf = true;
  Source(A, s, 16);
  for (int i = 0; i < 16; ++i) {
    if (s[i] & d[i]) zf = false;
    if (s[i] & ~d[i]) cf = false;
  }
  m->flags = SetFlag(m->flags, FLAGS_ZF, zf);
  m->flags = SetFlag(m->flags, FLAGS_CF, cf);
  m->flags = SetFlag(m->flags, FLAGS_AF, false);
  m->flags = SetFlag(m->flags, FLAGS_OF, false);
  m->flags = SetFlag(m->flags, FLAGS_PF, false);
  m->flags = SetFlag(m->flags, FLAGS_SF, false);
}

////////////////////////////////////////////////////////////////////////////////
// Lanes and general registers: pextrb, pextrw, pextrd, pextrq and
// extractps to a register, zero-extended, or to memory of the element's
// size; pinsrb, pinsrd and pinsrq from one; insertps between lanes.

static void Extract(P, const struct Sse4Op *op) {
  int size = ElementSize(op, rde);
  u64 x = LoadElement(XmmRexrReg(m, rde) + (uimm0 & (16 / size - 1)) * size, size, false);
  if (IsModrmRegister(rde)) {
    Put64(RegRexbRm(m, rde), x);
  } else {
    StoreElement(ComputeReserveAddressWrite(A, size), size, x);
  }
}

static void Insert(P, const struct Sse4Op *op) {
  int size = ElementSize(op, rde);
  u64 x = LoadElement(GetModrmRegisterWordPointerRead(A, size), size, false);
  StoreElement(XmmRexrReg(m, rde) + (uimm0 & (16 / size - 1)) * size, size, x);
}

static void Insertps(P, const struct Sse4Op *op) {
  u8 *d = XmmRexrReg(m, rde);
  u32 x = IsModrmRegister(rde)
              ? Get32(XmmRexbRm(m, rde) + ((uimm0 >> 6) & 3) * 4)
              : Get32(ComputeReserveAddressRead(A, 4));
  Put32(d + ((uimm0 >> 4) & 3) * 4, x);
  for (int i = 0; i < 4; ++i) {
    if (uimm0 & (1 << i)) Put32(d + i * 4, 0);
  }
}

////////////////////////////////////////////////////////////////////////////////
// Floating point: roundps, roundpd, roundss and roundsd, the mode imm8's
// low bits or, with its bit 2, MXCSR's, a scalar leaving the upper lanes;
// dpps and dppd, the products imm8's high bits pick summed pairwise as the
// hardware does into the lanes its low bits pick.

static double Round(double x, int mode) {
  switch (mode) {
    case 0:
      return rint(x);
    case 1:
      return floor(x);
    case 2:
      return ceil(x);
    default:
      return trunc(x);
  }
}

static void Rounds(P, const struct Sse4Op *op) {
  u8 s[16], *d = XmmRexrReg(m, rde);
  int mode = uimm0 & 4 ? (m->mxcsr & kMxcsrRc) >> 13 : uimm0 & 3;
  int n = op->round.scalar ? op->size : 16;
  Source(A, s, n);
  for (int i = 0; i < n; i += op->size) {
    if (op->size == 4) {
      union FloatPun f = {.i = Get32(s + i)};
      f.f = Round(f.f, mode);  // exact: a float rounds to a float
      Put32(d + i, f.i);
    } else {
      union DoublePun f = {.i = Get64(s + i)};
      f.f = Round(f.f, mode);
      Put64(d + i, f.i);
    }
  }
}

// An operation that makes a NaN of two numbers, infinity times zero or
// infinity less infinity, gives x86's indefinite, which is negative, where
// an Arm host's default NaN is positive. A NaN that was an operand goes
// through as the host passes it.
static float MakeNanX86f(float r, float a, float b) {
  return isnan(r) && !isnan(a) && !isnan(b) ? copysignf(r, -1) : r;
}

static double MakeNanX86d(double r, double a, double b) {
  return isnan(r) && !isnan(a) && !isnan(b) ? copysign(r, -1) : r;
}

static float Mulf(float a, float b) {
  return MakeNanX86f(a * b, a, b);
}

static float Addf(float a, float b) {
  return MakeNanX86f(a + b, a, b);
}

static double Muld(double a, double b) {
  return MakeNanX86d(a * b, a, b);
}

static double Addd(double a, double b) {
  return MakeNanX86d(a + b, a, b);
}

static void Dpps(P, const struct Sse4Op *op) {
  u8 s[16], *d = XmmRexrReg(m, rde);
  float p[4];
  Source(A, s, 16);
  for (int i = 0; i < 4; ++i) {
    union FloatPun a = {.i = Get32(d + i * 4)}, b = {.i = Get32(s + i * 4)};
    p[i] = uimm0 & (0x10 << i) ? Mulf(a.f, b.f) : 0;
  }
  union FloatPun sum = {.f = Addf(Addf(p[0], p[1]), Addf(p[2], p[3]))};
  for (int i = 0; i < 4; ++i) Put32(d + i * 4, uimm0 & (1 << i) ? sum.i : 0);
}

static void Dppd(P, const struct Sse4Op *op) {
  u8 s[16], *d = XmmRexrReg(m, rde);
  double p[2];
  Source(A, s, 16);
  for (int i = 0; i < 2; ++i) {
    union DoublePun a = {.i = Get64(d + i * 8)}, b = {.i = Get64(s + i * 8)};
    p[i] = uimm0 & (0x10 << i) ? Muld(a.f, b.f) : 0;
  }
  union DoublePun sum = {.f = Addd(p[0], p[1])};
  for (int i = 0; i < 2; ++i) Put64(d + i * 8, uimm0 & (1 << i) ? sum.i : 0);
}

static void Mpsadbw(P, const struct Sse4Op *op) {
  u8 s[16], r[16], *d = XmmRexrReg(m, rde);
  int soff = (uimm0 & 3) * 4, doff = ((uimm0 >> 2) & 1) * 4;
  Source(A, s, 16);
  for (int i = 0; i < 8; ++i) {
    unsigned sum = 0;
    for (int j = 0; j < 4; ++j) sum += abs(d[doff + i + j] - s[soff + j]);
    Put16(r + i * 2, sum);
  }
  memcpy(d, r, 16);
}

////////////////////////////////////////////////////////////////////////////////
// String compares: pcmpestri, pcmpestrm, pcmpistri and pcmpistrm. imm8
// gives the element format (bits 0-1), the aggregation (2-3), the polarity
// (4-5) and which end an index counts from or whether a mask is expanded
// (6). A is the register operand and B the r/m one, each as long as its
// register says (pcmpestr*) or up to its first zero element (pcmpistr*).
// An element past its operand's length is invalid, and comparing one gives
// what the SDM's table says for the aggregation.

enum { kEqualAny, kRanges, kEqualEach, kEqualOrdered };

struct Strings {
  const u8 *a, *b;
  int format, aggregation, n, la, lb;
};

static i64 Element(const struct Strings *s, const u8 *x, int i) {
  return s->format & 1 ? (i64)LoadElement(x + i * 2, 2, s->format & 2)
                       : (i64)LoadElement(x + i, 1, s->format & 2);
}

static int ImplicitLength(const struct Strings *s, const u8 *x) {
  int i = 0;
  while (i < s->n && Element(s, x, i)) ++i;
  return i;
}

static int ExplicitLength(const u8 *reg, u64 rde, int n) {
  i64 x = Rexw(rde) ? (i64)Get64(reg) : (i32)Get32(reg);
  if (x < 0) x = x == INT64_MIN ? n : -x;
  return x > n ? n : x;
}

// B[j] against A[i] through cmp, unless either is invalid.
static bool Compare(const struct Strings *s, int i, int j, int cmp) {
  bool av = i < s->la, bv = j < s->lb;
  if (!av || !bv) {
    if (!av && !bv) {
      return s->aggregation == kEqualEach || s->aggregation == kEqualOrdered;
    }
    return !av && s->aggregation == kEqualOrdered;
  }
  i64 x = Element(s, s->b, j), y = Element(s, s->a, i);
  return cmp < 0 ? x >= y : cmp > 0 ? x <= y : x == y;
}

static bool Match(const struct Strings *s, int j) {
  switch (s->aggregation) {
    case kEqualAny:
      for (int i = 0; i < s->n; ++i) {
        if (Compare(s, i, j, 0)) return true;
      }
      return false;
    case kRanges:
      for (int i = 0; i + 1 < s->n; i += 2) {
        if (Compare(s, i, j, -1) && Compare(s, i + 1, j, 1)) return true;
      }
      return false;
    case kEqualEach:
      return Compare(s, j, j, 0);
    default:
      for (int i = 0; i < s->n - j; ++i) {
        if (!Compare(s, i, j + i, 0)) return false;
      }
      return true;
  }
}

static void Pcmpstr(P, const struct Sse4Op *op) {
  u8 b[16];
  Source(A, b, 16);
  struct Strings s = {
      .a = XmmRexrReg(m, rde),
      .b = b,
      .format = uimm0 & 3,
      .aggregation = (uimm0 >> 2) & 3,
      .n = uimm0 & 1 ? 8 : 16,
  };
  bool lengths = op->strings.lengths;
  s.la = lengths ? ExplicitLength(m->ax, rde, s.n) : ImplicitLength(&s, s.a);
  s.lb = lengths ? ExplicitLength(m->dx, rde, s.n) : ImplicitLength(&s, s.b);
  unsigned res = 0;
  for (int j = 0; j < s.n; ++j) res |= (unsigned)Match(&s, j) << j;
  switch ((uimm0 >> 4) & 3) {
    case 1:
      res ^= (1u << s.n) - 1;
      break;
    case 3:
      res ^= (1u << s.lb) - 1;
      break;
    default:
      break;
  }
  if (op->strings.index) {
    int i = s.n;
    if (res) i = uimm0 & 0x40 ? 31 - __builtin_clz(res) : __builtin_ctz(res);
    Put64(m->cx, i);
  } else {
    u8 *x = m->xmm[0];
    memset(x, 0, 16);
    if (uimm0 & 0x40) {
      for (int i = 0; i < s.n; ++i) {
        if (res & (1u << i)) memset(x + i * (16 / s.n), 0xff, 16 / s.n);
      }
    } else {
      Put16(x, res);
    }
  }
  m->flags = SetFlag(m->flags, FLAGS_CF, res != 0);
  m->flags = SetFlag(m->flags, FLAGS_ZF, s.lb < s.n);
  m->flags = SetFlag(m->flags, FLAGS_SF, s.la < s.n);
  m->flags = SetFlag(m->flags, FLAGS_OF, res & 1);
  m->flags = SetFlag(m->flags, FLAGS_AF, false);
  m->flags = SetFlag(m->flags, FLAGS_PF, false);
}

////////////////////////////////////////////////////////////////////////////////
// The maps

static const struct Sse4Op kMap2[256] = {
    [0x10] = {Blendv1},
    [0x14] = {Blendv4},
    [0x15] = {Blendv8},
    [0x17] = {Ptest},
    [0x20] = {Widen21, .widen = {.sign = true}},
    [0x21] = {Widen41, .widen = {.sign = true}},
    [0x22] = {Widen81, .widen = {.sign = true}},
    [0x23] = {Widen42, .widen = {.sign = true}},
    [0x24] = {Widen82, .widen = {.sign = true}},
    [0x25] = {Widen84, .widen = {.sign = true}},
    [0x28] = {Pmuldq},
    [0x29] = {Lanewise8, .lanes = {.op = kEq}},
    [0x2b] = {Packusdw},
    [0x30] = {Widen21},
    [0x31] = {Widen41},
    [0x32] = {Widen81},
    [0x33] = {Widen42},
    [0x34] = {Widen82},
    [0x35] = {Widen84},
    [0x37] = {Lanewise8, .lanes = {.op = kGt, .sign = true}},
    [0x38] = {Lanewise1, .lanes = {.op = kMin, .sign = true}},
    [0x39] = {Lanewise4, .lanes = {.op = kMin, .sign = true}},
    [0x3a] = {Lanewise2, .lanes = {.op = kMin}},
    [0x3b] = {Lanewise4, .lanes = {.op = kMin}},
    [0x3c] = {Lanewise1, .lanes = {.op = kMax, .sign = true}},
    [0x3d] = {Lanewise4, .lanes = {.op = kMax, .sign = true}},
    [0x3e] = {Lanewise2, .lanes = {.op = kMax}},
    [0x3f] = {Lanewise4, .lanes = {.op = kMax}},
    [0x41] = {Phminposuw},
};

static const struct Sse4Op kMap3[256] = {
    [0x08] = {Rounds, .size = 4},
    [0x09] = {Rounds, .size = 8},
    [0x0a] = {Rounds, .size = 4, .round = {.scalar = true}},
    [0x0b] = {Rounds, .size = 8, .round = {.scalar = true}},
    [0x0c] = {Blend4},
    [0x0d] = {Blend8},
    [0x0e] = {Blend2},
    [0x14] = {Extract, .size = 1},
    [0x15] = {Extract, .size = 2},
    [0x16] = {Extract},
    [0x17] = {Extract, .size = 4},
    [0x20] = {Insert, .size = 1},
    [0x21] = {Insertps},
    [0x22] = {Insert},
    [0x40] = {Dpps},
    [0x41] = {Dppd},
    [0x42] = {Mpsadbw},
    [0x60] = {Pcmpstr, .strings = {.lengths = true}},
    [0x61] = {Pcmpstr, .strings = {.lengths = true, .index = true}},
    [0x62] = {Pcmpstr},
    [0x63] = {Pcmpstr, .strings = {.index = true}},
};

void OpSse4Map2(P) {
  Dispatch(A, kMap2);
}

void OpSse4Map3(P) {
  Dispatch(A, kMap3);
}
