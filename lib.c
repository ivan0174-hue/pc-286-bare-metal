#include "emu.h"
void *memset(void *d, int c, size_t n){ uint8_t *p=d; while(n--) *p++=c; return d; }
void *memcpy(void *d, const void *s, size_t n){ uint8_t *p=d; const uint8_t *q=s; while(n--) *p++=*q++; return d; }
void *memmove(void *d, const void *s, size_t n){ uint8_t *p=d; const uint8_t *q=s;
    if(p<q) while(n--) *p++=*q++; else { p+=n; q+=n; while(n--) *--p=*--q; } return d; }
int memcmp(const void *a, const void *b, size_t n){ const uint8_t *p=a,*q=b; for(;n;n--,p++,q++) if(*p!=*q) return *p-*q; return 0; }
