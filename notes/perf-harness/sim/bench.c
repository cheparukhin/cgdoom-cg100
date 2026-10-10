typedef unsigned char u8; typedef unsigned int u32;
void col_asm(u8 *dest, const u8 *src, const u8 *cm2, int count, u32 frac9, u32 step9);
void col_asm_rpt(u8 *dest, const u8 *src, const u8 *cm2, int count, u32 frac9, u32 step9);
static u8 fb_ref[320*200], fb_new[320*200];
static u8 tex[128*64];          /* 64 columns of 128 texels */
static u8 cmap[256];
static u8 cm2buf[512];
static u32 seed = 1;
static u32 rndn(u32 n);
static u32 rnd(void){ seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }
/* vanilla R_DrawColumn inner loop (C reference, as in CGDoom r_draw.c:137-146) */
__attribute__((noinline)) static void col_c(u8 *dest, const u8 *src, const u8 *cm, int count, int frac, int fracstep){
  count--; do { *dest = cm[src[(frac>>16)&127]]; dest += 320; frac += fracstep; } while(count--);
}
#ifndef MODE
#define MODE 0
#endif
#ifndef REPS
#define REPS 2000
#endif
static u32 rndn(u32 n){ return ((rnd() & 0xffff) * n) >> 16; }
int main(void){
  for(int i=0;i<128*64;i++) tex[i]=(u8)rnd();
  for(int i=0;i<256;i++) cmap[i]=(u8)(rnd());
  for(int i=0;i<256;i++){ cm2buf[i]=cmap[i]; cm2buf[256+i]=cmap[i]; }
  const u8 *cm2 = cm2buf + 256;
  int bad = 0; long pixels = 0;
  for(int r=0;r<REPS;r++){
    int x = rndn(320), yl = rndn(200), yh = yl + (int)rndn(200-yl);
    int count = yh-yl+1; pixels += count;
    int frac = (int)rnd(); int step = (int)(rnd() & 0x3ffff) + 0x1000;
    const u8 *src = tex + 128*(rnd()&63);
    if(MODE==0 || MODE==9) col_c(fb_ref + yl*320 + x, src, cmap, count, frac, step);
    if(MODE==1 || MODE==9) col_asm(fb_new + yl*320 + x, src, cm2, count, (u32)frac<<9, (u32)step<<9);
    if(MODE==2 || MODE==8) col_asm_rpt(fb_new + yl*320 + x, src, cm2, count, (u32)frac<<9, (u32)step<<9);
    if(MODE==8) col_c(fb_ref + yl*320 + x, src, cmap, count, frac, step);
  }
  if(MODE==9 || MODE==8) for(int i=0;i<320*200;i++) bad += fb_ref[i]!=fb_new[i];
  return bad ? 100 + (bad > 100 ? 100 : bad) : 0;
}
