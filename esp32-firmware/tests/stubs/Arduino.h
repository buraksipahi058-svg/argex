#pragma once
#include <cstdint>
#include <cstddef>
#include <deque>
#include <vector>
#define CONFIG_IDF_TARGET_ESP32 1
#define HIGH 1
#define LOW 0
#define OUTPUT 1
#define SERIAL_8N1 0
inline uint32_t clock_ms=0;
inline uint32_t millis(){return clock_ms;}
inline void delay(unsigned x){clock_ms+=x;}
inline void pinMode(int,int){}
inline int levels[40]={};
inline void digitalWrite(int pin,int v){if(pin>=0 && pin<40)levels[pin]=v;}
inline uint32_t duty[40]={};
inline bool ledcAttach(int,int,int){return true;}
inline bool ledcWrite(int pin,uint32_t v){duty[pin]=v;return true;}
struct HardwareSerial {
 std::deque<uint8_t> rx; std::vector<uint8_t> tx;
 HardwareSerial(int=0){}
 void begin(unsigned long,int=0,int=-1,int=-1){}
 void setRxBufferSize(int){} void setTxBufferSize(int){}
 int available(){return rx.size();} int availableForWrite(){return 1024;}
 int read(){if(rx.empty())return -1;int v=rx.front();rx.pop_front();return v;}
 size_t write(const uint8_t*p,size_t n){tx.insert(tx.end(),p,p+n);return n;}
 void println(const char* = ""){}
 template<class...T>void printf(const char*,T...){}
};
inline HardwareSerial Serial, Serial2;
