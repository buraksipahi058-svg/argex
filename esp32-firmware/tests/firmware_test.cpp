#define IMU_ENABLED 0   /* host testleri IMU/I2C icermez; IMU yolu ayri smoke-compile edilir */
#include <cassert>
#include <iostream>
#include "../ika_esp32/controller.cpp"
static uint8_t seq=0;
void radio(int ch1=992,int ch2=992,int ch5=1811,int ch6=172,int ch8=172,int ch10=172){
 uint16_t channels[16];for(auto &c:channels)c=992;
 channels[0]=ch1;channels[1]=ch2;channels[4]=ch5;channels[5]=ch6;channels[7]=ch8;channels[9]=ch10;
 uint8_t body[24]={0x16};
 for(int c=0;c<16;c++)for(int b=0;b<11;b++)if((channels[c]>>b)&1)body[1+(c*11+b)/8]|=1<<((c*11+b)%8);
 body[23]=Crsf_Crc8(body,23);Serial2.rx.push_back(0xc8);Serial2.rx.push_back(24);
 for(auto b:body)Serial2.rx.push_back(b);
}
void command_frame(int left=0,int right=0,bool laser=false,bool enabled=true,int version=1,bool corrupt=false){
 uint8_t b[]={uint8_t(version),2,7,seq++,uint8_t(left),uint8_t(right),90,90,uint8_t(laser),255,uint8_t(enabled)};
 auto crc=Proto_Crc16(b,sizeof b);if(corrupt)crc^=1;
 JetsonSerial.rx.push_back(0xaa);JetsonSerial.rx.push_back(0x55);
 for(auto v:b)JetsonSerial.rx.push_back(v);
 JetsonSerial.rx.push_back(crc&255);JetsonSerial.rx.push_back(crc>>8);
}
void step(bool rc=true,int mode=0,int throttle=992,int fire=172,bool cmd=false,int speed=0){
 clock_ms+=10;
 if(rc)radio(992,throttle,mode==1?172:1811,172,mode==2?1811:172,fire);
 if(cmd)command_frame(speed,speed);
 ikaLoop();
}
void settle(int mode,int n=65,bool cmd=false){for(int i=0;i<n;i++)step(true,mode,992,172,cmd);}
int main(){
 ikaSetup();assert(!g_motor_armed&&!g_fire_on);assert(duty[18]==0&&duty[19]==0);
 for(int i=0;i<200;i++)step(true,0,1811);assert(!g_motor_armed&&g_applied_left==0);
 settle(0);assert(g_motor_armed);
 step(true,0,1811);assert(g_applied_left>0&&g_applied_left<=300&&g_fire_on==0);
 auto pan=g_pan_us;step(true,0,1811,1811);assert(g_pan_us==pan&&g_fire_on==0);
 // Transition while held joystick/fire: motors stop; turret and laser must wait.
 step(true,1,1811,1811);assert(!g_motor_armed&&g_applied_left==0&&!g_turret_armed&&!g_fire_on);
 for(int i=0;i<60;i++)step(true,1,992,1811);assert(g_turret_armed&&!g_fire_on);
 step(true,1,992,172);step(true,1,992,1811);assert(g_fire_on);
 for(int i=0;i<8;i++)step(true,1,1811,1811);assert(g_applied_left==0&&g_tilt_us<TILT_CENTER_US);
 step(true,1,992,172);assert(!g_fire_on);
 // RC timeout clears drive and laser; no automatic drive resumption on deflected stick.
 for(int i=0;i<31;i++)step(false);assert(!g_fire_on&&g_applied_left==0);
 for(int i=0;i<70;i++)step(true,0,1811);assert(!g_motor_armed);
 settle(0);assert(g_motor_armed);
 // CH8 overrides either CH5 state; no Jetson = STOP.
 settle(2);assert(g_mode==MODE_AUTONOMOUS&&!g_motor_armed&&g_applied_left==0);
 step(true,2,992,172,true,20);assert(g_motor_armed&&g_applied_left==200);
 step(true,2,1811,1811,true,20);assert(g_applied_left==200&&!g_fire_on);
 // Timeout even with radio connected.
 settle(2,21,false);assert(!g_motor_armed&&g_applied_left==0);
 // Bad CRC and wrong protocol versions cannot refresh command freshness.
 auto last=g_cmd.sonAlim;command_frame(0,0,false,true,2);Proto_Poll(clock_ms);assert(last==g_cmd.sonAlim);
 command_frame(0,0,false,true,1,true);Proto_Poll(clock_ms);assert(last==g_cmd.sonAlim);
 command_frame(110,0);Proto_Poll(clock_ms);assert(last==g_cmd.sonAlim);
 // A valid frame accepted once, then duplicate sequence ignored.
 command_frame();Proto_Poll(clock_ms);last=g_cmd.sonAlim;seq--;clock_ms+=10;
 command_frame(50,50);Proto_Poll(clock_ms);assert(last==g_cmd.sonAlim);
 settle(2,65,true);assert(g_motor_armed);
 step(true,2,992,172,true,20);assert(g_applied_left==200);
 step(true,1,992);assert(g_applied_left==0&&!g_cmd_received&&!g_fire_on);
 // All motor encodings stay in their channel; never zero; stop values correct.
 for(int i=-1000;i<=1000;i++){assert(Reactor_ByteA(i)>=1&&Reactor_ByteA(i)<=127);assert(Reactor_ByteB(i)>=128);}
 assert(Reactor_ByteA(0)==64&&Reactor_ByteB(0)==192);
 Reactor_SetDrive(1000,-1000);assert(sent_bytes[25][0]==127&&sent_bytes[25][1]==255);
 // Explicit CRSF LQ=0 invalidates an otherwise fresh channel frame.
 g_lq_seen=true;g_lq=0;assert(!Crsf_IsLinkUp(&g_crsf,clock_ms));g_lq=100;
 // Hardware TX failure latches fault and removes software enable.
 settle(0);rmt_fail=true;step(true,0,1811);assert(g_hw_fault&&!g_motor_armed&&!g_fire_on&&g_applied_left==0);
 std::cout<<"PASS: actual firmware mode, arming, timeout, protocol, RMT framing, link and fault tests\n";
 return 0;
}
