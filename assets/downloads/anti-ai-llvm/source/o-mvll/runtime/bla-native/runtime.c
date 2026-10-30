// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#include <stdint.h>
#include <stddef.h>
#if defined(BLA_TEST_API)
#define VIS __attribute__((visibility("default")))
#else
#define VIS __attribute__((visibility("hidden")))
#endif
static unsigned lower(unsigned c) { return c>='A'&&c<='Z'?c+32:c; }
static int contains(const char *p,size_t n,const char *needle) {
  for(size_t i=0;i<n;i++) {
    size_t j=0;
    while(needle[j]&&i+j<n&&lower((unsigned char)p[i+j])==(unsigned char)needle[j])++j;
    if(!needle[j])return 1;
  }
  return 0;
}
// Two distinct observations; no timing heuristics or process mutation.
VIS uint32_t __omvll_bla_classify(const char *status,size_t n,const char *maps,size_t m) {
  uint32_t flags=0;
  static const char field[]="TracerPid:";
  for(size_t i=0;i+sizeof(field)-1<=n;i++) {
    if(i&&status[i-1]!='\n')continue;
    size_t j=0;while(j<sizeof(field)-1&&status[i+j]==field[j])++j;
    if(j!=sizeof(field)-1)continue;
    for(size_t k=i+j;k<n&&status[k]!='\n';k++)if(status[k]>='1'&&status[k]<='9')flags|=1;
  }
  if(contains(maps,m,"frida-agent")||contains(maps,m,"frida-gadget")||
     contains(maps,m,"libfrida")||contains(maps,m,"gum-js-loop"))flags|=2;
  return flags;
}
#if defined(__ANDROID__) && defined(__aarch64__)
#include <stdatomic.h>
static long sys4(long nr,long a,long b,long c,long d) {
  register long x8 __asm__("x8")=nr;
  register long x0 __asm__("x0")=a;
  register long x1 __asm__("x1")=b;
  register long x2 __asm__("x2")=c;
  register long x3 __asm__("x3")=d;
  __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3):"memory","cc");
  return x0;
}
static uint32_t probe_now(void) {
  char status[4096];
  long fd=sys4(56,-100,(long)"/proc/self/status",0,0);
  long n=0;
  if(fd>=0){n=sys4(63,fd,(long)status,sizeof(status),0);sys4(57,fd,0,0,0);}
  uint32_t flags=n>0?__omvll_bla_classify(status,(size_t)n,"",0):0;
  fd=sys4(56,-100,(long)"/proc/self/maps",0,0);
  if(fd<0)return flags; // Unavailable observations do not block normal behavior.
  char buf[1088];size_t carry=0;
  for(unsigned round=0;round<32;round++) {
    long got=sys4(63,fd,(long)(buf+carry),1024,0);
    if(got<=0)break;
    size_t total=carry+(size_t)got;
    flags|=__omvll_bla_classify("",0,buf,total);
    if(flags&2)break;
    carry=total<64?total:64;
    for(size_t i=0;i<carry;i++)buf[i]=buf[total-carry+i];
  }
  sys4(57,fd,0,0,0);
  return flags;
}
VIS __attribute__((noinline)) uint32_t __omvll_bla_runtime_probe(void) {
  static _Atomic uint32_t calls=0;
  static _Atomic uint32_t last=0;
  uint32_t ticket=atomic_fetch_add_explicit(&calls,1,memory_order_relaxed);
  if(ticket&63u)return atomic_load_explicit(&last,memory_order_relaxed);
  uint32_t flags=probe_now();
  atomic_store_explicit(&last,flags,memory_order_relaxed);
  return flags;
}
#else
// macOS native equivalence tests do not pretend to observe an Android process.
VIS __attribute__((noinline)) uint32_t __omvll_bla_runtime_probe(void) { return 0; }
#endif
