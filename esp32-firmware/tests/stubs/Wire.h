#pragma once
// Minimal Arduino Wire (I2C) stub for host tests. BNO055 okumasi burada
// gecerli cip ID vermez; Bno_Init false doner ve IMU yolu host'ta pasif kalir.
#include <cstdint>
#include <cstddef>
struct TwoWire {
  void begin(int = -1, int = -1, uint32_t = 0) {}
  void setClock(uint32_t) {}
  void setTimeOut(uint16_t) {}
  void beginTransmission(uint8_t) {}
  size_t write(uint8_t) { return 1; }
  uint8_t endTransmission(bool = true) { return 0; }
  uint8_t requestFrom(int, int n) { return (uint8_t)n; }
  int read() { return 0; }
  int available() { return 0; }
};
inline TwoWire Wire;
