#include <stdio.h>
#include "ppu_rsp.h"
int main(int argc, char **argv) {
  static PpuFrame f; long n = 0, simple = 0, sub = 0, b15 = 0, win = 0, nl = 0, maxl = 0;
  for (int a = 1; a < argc; a++) { FILE *fp = fopen(argv[a], "rb"); int32_t h[2];
    while (fread(h, 4, 2, fp) == 2 && fread(&f, sizeof f, 1, fp) == 1) { int cnt = 0;
      for (int l = 0; l < f.numLines; l++) { PpuLineRec *r = &f.lines[l];
        if (!(r->flags & kLR_Valid) || (r->flags & (kLR_CpuDrawn | kLR_ForcedBlank))) continue;
        n++; cnt++; int pm = r->mathEnabled ? r->preventMathMode : 3;
        if (pm == 3 && r->clipMode == 0) simple++;
        if (pm != 3 && (r->mathFlags & 1) && r->screenEnabled[1]) sub++;
        if (r->brightness == 15) b15++;
        if (r->screenWindowed[0] | r->screenWindowed[1]) win++; }
      if (cnt > maxl) maxl = cnt; nl++; } }
  printf("drawn lines %ld: simple(no math,no clip) %.0f%%, subscreen %.0f%%, bright15 %.0f%%, windowed %.0f%%, max lines/frame %ld\n",
    n, 100.0*simple/n, 100.0*sub/n, 100.0*b15/n, 100.0*win/n, maxl);
}
