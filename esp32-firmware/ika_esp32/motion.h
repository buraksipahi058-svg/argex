#pragma once
#include <stdint.h>
// Pure control logic; exercised by host tests as well as the firmware.
static inline int32_t motionAbs(int32_t x) { return x < 0 ? -x : x; }
struct ServoInputFilter {
  int32_t samples[3] = {0,0,0};
  uint8_t index=0;
  int32_t filtered=0;
  bool active=false;
  void reset() { for(auto &s:samples)s=0; index=0; filtered=0; active=false; }
  int32_t update(int32_t offset_us, int32_t enter_us, int32_t exit_us) {
    samples[index]=offset_us; index=(index+1)%3;
    int32_t a=samples[0],b=samples[1],c=samples[2],t;
    if(a>b){t=a;a=b;b=t;} if(b>c){t=b;b=c;c=t;} if(a>b){t=a;a=b;b=t;}
    // Immediate centre stop; median rejects isolated outliers outside centre.
    if(motionAbs(offset_us)<=exit_us) { reset();return 0; }
    if(motionAbs(b)<=exit_us) {
      active=false; filtered=0; return 0;
    }
    if(!active && motionAbs(b)<enter_us){filtered=0;return 0;}
    active=true;
    int32_t target=(b>0?b-exit_us:b+exit_us)*1000/(512-exit_us);
    if(target>1000)target=1000;if(target< -1000)target=-1000;
    filtered+=(target-filtered)/4;
    return filtered;
  }
};

struct ManualDriveProfile {
  enum Heading : uint8_t { FORWARD, BACKWARD, RIGHT, LEFT, NONE };
  Heading heading=NONE;
  int32_t magnitude_q=0; // command per-mille * 1000
  uint32_t last_ms=0,zero_ms=0;
  bool started=false,zero_seen=false,pivot=false;
  int16_t left=0,right=0;
  void reset() { *this=ManualDriveProfile(); }
  void update(int32_t throttle,int32_t steer,int32_t gear_pct,uint32_t now,
              int32_t turn_gain,int32_t turn_cap,int32_t enter,int32_t exit,
              int32_t accel_pct_s,int32_t decel_pct_s,uint32_t hold_ms) {
    uint32_t dt=started?(uint32_t)(now-last_ms):10; started=true;last_ms=now;
    if(dt>50)dt=50; // stalled control loop must not produce a large speed jump
    if(pivot){if(motionAbs(steer)<=exit)pivot=false;}
    else if(motionAbs(steer)>=enter)pivot=true;
    Heading requested=NONE;
    int32_t target=0;
    if(pivot){
      requested=steer>0?RIGHT:LEFT;
      target=motionAbs(steer)*gear_pct*turn_gain/10000;
      if(target>turn_cap*10)target=turn_cap*10;
    }else if(throttle){
      requested=throttle>0?FORWARD:BACKWARD;
      target=motionAbs(throttle)*gear_pct/100;
    }
    if(heading==NONE && requested!=NONE) heading=requested;
    bool change=requested!=NONE && requested!=heading;
    if(change && magnitude_q==0 && (!zero_seen || (uint32_t)(now-zero_ms)>=hold_ms)) {
      heading=requested;change=false;
    }
    if(change)target=0;
    int32_t goal=target*1000;
    int32_t rate=(goal>magnitude_q?accel_pct_s:decel_pct_s)*10;
    int32_t delta=rate*(int32_t)dt;
    int32_t before=magnitude_q;
    if(magnitude_q<goal){magnitude_q+=delta;if(magnitude_q>goal)magnitude_q=goal;}
    else if(magnitude_q>goal){magnitude_q-=delta;if(magnitude_q<goal)magnitude_q=goal;}
    if(before>0 && magnitude_q==0){zero_ms=now;zero_seen=true;}
    if(magnitude_q>0)zero_seen=false;
    int16_t v=(int16_t)(magnitude_q/1000);
    switch(heading){
      case FORWARD:left=v;right=v;break;
      case BACKWARD:left=-v;right=-v;break;
      case RIGHT:left=v;right=-v;break;
      case LEFT:left=-v;right=v;break;
      default:left=right=0;
    }
  }
};
