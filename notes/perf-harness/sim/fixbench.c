typedef int fixed_t; typedef unsigned int u32; typedef unsigned long long u64;
#define FRACBITS 16
#define MAXINT 0x7fffffff
#define MININT ((int)0x80000000)
static inline int iabs(int x){ return x < 0 ? -x : x; }
/* ---- CGDoom m_fixed.c (verbatim logic, abs() -> iabs) ---- */
__attribute__((noinline)) fixed_t CGD_FixedMul( fixed_t a, fixed_t b ){
  int iResult; unsigned int iNegative = (a<0) != (b <0); unsigned int iMask = 1;
  unsigned int iResLo = 0, iResHi = 0, iaHi = 0; unsigned int ua = iabs(a), ub = iabs(b);
  if(ua<ub){ unsigned int uc = ua; ua = ub; ub = uc; }
  while(ub){ if(ub & iMask){ unsigned int iLo = iResLo + ua; if(iLo < iResLo) iResHi++; iResLo = iLo; iResHi += iaHi; ub &= ~iMask; }
    iMask <<= 1; iaHi <<= 1; if(ua & 0x80000000) iaHi |= 1; ua <<= 1; }
  iResLo >>= FRACBITS; iResHi <<= FRACBITS; iResult = iResHi | iResLo; if(iNegative) iResult= -iResult; return iResult; }
__attribute__((noinline)) unsigned int FixedDivU(unsigned int ua, unsigned int ub ){
  unsigned int uRes = 0; for(int i=0;i<FRACBITS;i++){ unsigned int uDiv = ua / ub; ua = ua % ub; uRes += uDiv; uRes <<= 1; ua <<= 1; } return uRes; }
__attribute__((noinline)) fixed_t CGD_FixedDiv( fixed_t a, fixed_t b ){
  int iResult; int iNegative = (a < 0) != (b<0);
  if ( (iabs(a)>>14) >= iabs(b)) return iNegative ? MININT : MAXINT;
  iResult = FixedDivU(iabs(a),iabs(b)); if(iNegative) iResult = -iResult; return iResult; }
/* ---- proposed ---- */
__attribute__((noinline)) fixed_t New_FixedMul(fixed_t a, fixed_t b){ return (fixed_t)(((long long)a * b) >> FRACBITS); }
static inline u32 udiv64_32(u32 hi, u32 lo, u32 d){
  __asm__("div0u\n\t.rept 32\n\trotcl %1\n\tdiv1 %2,%0\n\t.endr\n\trotcl %1" : "+r"(hi), "+r"(lo) : "r"(d) : "t");
  return lo; }
__attribute__((noinline)) fixed_t New_FixedDiv(fixed_t a, fixed_t b){
  if ((iabs(a)>>14) >= iabs(b)) return (a^b)<0 ? MININT : MAXINT;
  u32 ua = (u32)iabs(a), ub = (u32)iabs(b);
  u32 q = udiv64_32(ua >> 16, ua << 16, ub);
  return (a^b) < 0 ? -(int)q : (int)q; }
/* exact reference: restoring long division of (|a|<<16) by |b|, no libcalls */
static fixed_t Ref_FixedDiv(fixed_t a, fixed_t b){
  if ((iabs(a)>>14) >= iabs(b)) return (a^b)<0 ? MININT : MAXINT;
  u64 n = (u64)(u32)iabs(a) << 16, d = (u32)iabs(b), r = 0; u32 q = 0;
  for(int i = 47; i >= 0; i--){ r = (r << 1) | ((n >> i) & 1); q <<= 1; if(r >= d){ r -= d; q |= 1; } }
  return (a^b) < 0 ? -(int)q : (int)q; }
static u32 seed = 99; static u32 rnd(void){ seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }
/* operands shaped like Doom's: magnitudes of 8..26 significant bits, random sign */
static int opnd(void){ int bits = 8 + (int)(((rnd() & 0xffff) * 19) >> 16); int v = (int)(rnd() & ((1u << bits) - 1)) | (1 << (bits-1)); return (rnd() & 1) ? -v : v; }
#ifndef MODE
#define MODE 0
#endif
#define N 4000
volatile int sink;
int main(void){
  int bad = 0;
  for(int i = 0; i < N; i++){
    int a = opnd(), b = opnd();
#if MODE == 1
    sink = CGD_FixedMul(a, b);
#elif MODE == 2
    sink = New_FixedMul(a, b);
#elif MODE == 3
    sink = CGD_FixedDiv(a, b);
#elif MODE == 4
    sink = New_FixedDiv(a, b);
#elif MODE == 9
    if(New_FixedDiv(a, b) != Ref_FixedDiv(a, b)) bad++;
    if(New_FixedMul(a, b) != (fixed_t)(((long long)a*b)>>16)) bad++;
#else
    sink = a ^ b;
#endif
  }
  return bad > 200 ? 200 : bad;
}
