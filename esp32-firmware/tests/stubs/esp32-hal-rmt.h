#pragma once
#include <cassert>
#define RMT_TX_MODE 1
#define RMT_MEM_NUM_BLOCKS_1 1
struct rmt_data_t{unsigned duration0:15;unsigned level0:1;unsigned duration1:15;unsigned level1:1;};
inline bool rmt_fail=false;
inline uint8_t sent_bytes[40][2]={};
inline bool rmtInit(int,int,int,unsigned){return true;}
inline bool rmtSetEOT(int,int){return true;}
inline bool rmtWrite(int pin,rmt_data_t*p,size_t n,unsigned){
 assert(n==10);unsigned ticks=0;
 for(int byte=0;byte<2;byte++){
  uint16_t bits=0;
  for(int j=0;j<5;j++){
   auto s=p[byte*5+j]; bits|=s.level0<<(2*j);bits|=s.level1<<(2*j+1);
   assert(s.duration0==260||s.duration0==261);assert(s.duration1==260||s.duration1==261);
   ticks+=s.duration0+s.duration1;
  }
  assert((bits&1)==0 && (bits&512));sent_bytes[pin][byte]=(bits>>1)&255;
 }
 assert(ticks==5208);return !rmt_fail;
}
