typedef unsigned int u32;
static u32 seed = 5; static u32 rnd(void){ seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }
volatile u32 sink;
int main(void){ for(int i=0;i<4000;i++){ int bits = 9 + (int)(((rnd()&0xffff)*14)>>16); u32 s = (rnd() & ((1u<<bits)-1)) | (1u<<(bits-1));
#if MODE==1
 sink = 0xffffffffu / s;
#else
 sink = s;
#endif
 } return 0; }
