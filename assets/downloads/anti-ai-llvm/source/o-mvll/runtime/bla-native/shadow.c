// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#include <stdint.h>
#define H __attribute__((noinline,optnone,visibility("hidden")))
H static uint32_t stage_0(uint32_t,uint32_t);
H static uint32_t stage_1(uint32_t,uint32_t);
H static uint32_t stage_2(uint32_t,uint32_t);
H static uint32_t stage_3(uint32_t,uint32_t);
H static uint32_t stage_4(uint32_t,uint32_t);
H static uint32_t stage_0(uint32_t s,uint32_t depth) {
  if(depth>4)depth=4;
  s=(s^0x91e573abu)*0x9e3779b9u;
  if(!depth)return s;
  uint32_t (*next)(uint32_t,uint32_t)=(s&1)?stage_1:stage_2;
  return next(s^(s>>13),depth-1);
}
H static uint32_t stage_1(uint32_t s,uint32_t depth) {
  if(depth>4)depth=4;
  s=(s^0x9308b912u)*0x9e3779b9u;
  if(!depth)return s;
  uint32_t (*next)(uint32_t,uint32_t)=(s&1)?stage_2:stage_3;
  return next(s^(s>>13),depth-1);
}
H static uint32_t stage_2(uint32_t s,uint32_t depth) {
  if(depth>4)depth=4;
  s=(s^0x942bfe79u)*0x9e3779b9u;
  if(!depth)return s;
  uint32_t (*next)(uint32_t,uint32_t)=(s&1)?stage_3:stage_4;
  return next(s^(s>>13),depth-1);
}
H static uint32_t stage_3(uint32_t s,uint32_t depth) {
  if(depth>4)depth=4;
  s=(s^0x954f43e0u)*0x9e3779b9u;
  if(!depth)return s;
  uint32_t (*next)(uint32_t,uint32_t)=(s&1)?stage_4:stage_0;
  return next(s^(s>>13),depth-1);
}
H static uint32_t stage_4(uint32_t s,uint32_t depth) {
  if(depth>4)depth=4;
  s=(s^0x96728947u)*0x9e3779b9u;
  if(!depth)return s;
  uint32_t (*next)(uint32_t,uint32_t)=(s&1)?stage_0:stage_1;
  return next(s^(s>>13),depth-1);
}
#if defined(BLA_TEST_API)
#define ENTRY __attribute__((noinline,optnone,visibility("default")))
#else
#define ENTRY H
#endif
ENTRY uint32_t __omvll_bla_shadow_eval(uint32_t user,uint32_t quantity,uint32_t tier,uint32_t token) {
  uint32_t state=stage_0(user^token,4),price=0,pc=state&3;
  for(unsigned step=0;step<6;step++) {
    switch(pc) {
      case 0: price=149u+130u*(tier%3u);pc=1;break;
      case 1: price*=1u+(quantity%100u);pc=2;break;
      case 2: price=(price*93u)/100u;pc=3;break;
      default: price^=state&15u;pc=0;break;
    }
    state=(state<<7)|(state>>25);
  }
  return price&0x7fffffffu;
}
