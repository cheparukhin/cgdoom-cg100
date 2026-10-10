#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#define FRACBITS 16
#define MAXINT 0x7fffffff
#define MININT ((int)0x80000000)
typedef int fixed_t;
/* CGDoom m_fixed.c (verbatim logic) */
static fixed_t CGD_FixedMul( fixed_t a, fixed_t b ){
  int iResult; unsigned int iNegative = (a<0) != (b <0); unsigned int iMask = 1;
  unsigned int iResLo = 0, iResHi = 0, iaHi = 0; unsigned int ua = abs(a), ub = abs(b);
  if(ua<ub){ unsigned int uc = ua; ua = ub; ub = uc; }
  while(ub){ if(ub & iMask){ unsigned int iLo = iResLo + ua; if(iLo < iResLo) iResHi++; iResLo = iLo; iResHi += iaHi; ub &= ~iMask; }
    iMask <<= 1; iaHi <<= 1; if(ua & 0x80000000) iaHi |= 1; ua <<= 1; }
  iResLo >>= FRACBITS; iResHi <<= FRACBITS; iResult = iResHi | iResLo; if(iNegative) iResult= -iResult; return iResult; }
static unsigned int FixedDivU(unsigned int ua, unsigned int ub ){
  unsigned int uRes = 0; for(int i=0;i<FRACBITS;i++){ unsigned int uDiv = ua / ub; ua = ua % ub; uRes += uDiv; uRes <<= 1; ua <<= 1; } return uRes; }
static fixed_t CGD_FixedDiv( fixed_t a, fixed_t b ){
  int iResult; int iNegative = (a < 0) != (b<0);
  if ( (abs(a)>>14) >= abs(b)) return iNegative ? MININT : MAXINT;
  iResult = FixedDivU(abs(a),abs(b)); if(iNegative) iResult = -iResult; return iResult; }
/* vanilla / chocolate */
static fixed_t V_FixedMul(fixed_t a, fixed_t b){ return (fixed_t)(((int64_t)a * b) >> FRACBITS); }
static fixed_t V_FixedDiv2(fixed_t a, fixed_t b){ double c = ((double)a) / ((double)b) * 65536.0; return (fixed_t)c; }
static fixed_t V_FixedDiv(fixed_t a, fixed_t b){ if ((abs(a)>>14) >= abs(b)) return (a^b)<0 ? MININT : MAXINT; return V_FixedDiv2(a,b); }
static fixed_t C_FixedDiv(fixed_t a, fixed_t b){ if ((abs(a)>>14) >= abs(b)) return (a^b)<0 ? MININT : MAXINT; return (fixed_t)(((int64_t)a << 16) / b); }
static uint32_t s=12345; static uint32_t rnd(void){ s^=s<<13; s^=s>>17; s^=s<<5; return s; }
static int rv(void){ int sh = rnd()%31; int v = (int)(rnd() >> sh); return (rnd()&1)? -v : v; }
int main(void){
  long N=20000000, mulmis=0, mulmis_neg=0, divmis=0, divmis_c=0, cdivmis=0;
  for(long i=0;i<N;i++){ int a=rv(), b=rv(); if(a==MININT||b==MININT) continue;
    if(CGD_FixedMul(a,b)!=V_FixedMul(a,b)){ mulmis++; if((a<0)!=(b<0)) mulmis_neg++; }
    if(b!=0){ if(CGD_FixedDiv(a,b)!=V_FixedDiv(a,b)) divmis++; if(C_FixedDiv(a,b)!=V_FixedDiv(a,b)) divmis_c++; }
  }
  printf("N=%ld  CGD FixedMul != vanilla: %ld (%.2f%%), of which opposite-sign: %ld\n", N, mulmis, 100.0*mulmis/N, mulmis_neg);
  printf("CGD FixedDiv != vanilla(double): %ld (%.4f%%)\n", divmis, 100.0*divmis/N);
  printf("int64 FixedDiv != vanilla(double): %ld (%.6f%%)\n", divmis_c, 100.0*divmis_c/N);
  return 0; }
