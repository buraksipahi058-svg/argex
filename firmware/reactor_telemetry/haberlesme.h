#ifndef HABERLESME_H
#define HABERLESME_H

#include <Arduino.h>

// ============================================================
//  STM32 <-> Jetson BINARY UART PROTOKOLU  (v1)
//  Frame: HDR0 HDR1 | VERSION | TYPE | LENGTH | SEQ | PAYLOAD | CRC_L CRC_H
//  - Cok baytli alanlar LITTLE-ENDIAN
//  - CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF), kapsam: VERSION..PAYLOAD
// ============================================================

// ---- Sabitler --------------------------------------------------------------
#define PROTO_HDR0        0xAA
#define PROTO_HDR1        0x55
#define PROTO_VERSION     0x01
#define PROTO_MAX_PAYLOAD 32       // ileri surumler icin bol

// Paket tipleri
#define TYPE_STATUS       0x01     // STM -> Jetson
#define TYPE_COMMAND      0x02     // Jetson -> STM
#define TYPE_HEARTBEAT    0x03     // cift yonlu
// (ileride: 0x04 ACK, 0x05 CONFIG, 0x06 LOG ...)

// Heartbeat kaynak alani
#define HB_KAYNAK_STM     0x00
#define HB_KAYNAK_JETSON  0x01

// ---- Zamanlama (ms) --------------------------------------------------------
#define STATUS_PERIOD_MS        50    // 20 Hz telemetri
#define HB_PERIOD_MS            100    // 10 Hz heartbeat
#define CMD_TIMEOUT_MS         200    // COMMAND bayatlama esigi
#define JETSON_HB_TIMEOUT_MS   500    // Jetson link kopus esigi

// ---- COMMAND bayrak bitleri (COMMAND payload byte 6) -----------------------
#define CMD_FLAG_AUTO_REQ     0x01    // Jetson otonom kontrol istiyor

// ---- STATUS durum bayragi bitleri (STATUS payload byte 7) ------------------
#define ST_JETSON_LINK        0x01    // Jetson linki taze
#define ST_CMD_TIMEOUT        0x02    // otonom komut bayat
#define ST_AUTO_EN            0x04    // otonom kontrol su an aktif
#define ST_FAILSAFE           0x08    // guvenlik motorlari durdurdu
#define ST_CRC_ERR            0x10    // yakin zamanda CRC hatasi
// bit5-7 reserved

// ---- Mod komut ozel degeri -------------------------------------------------
#define MOD_KOMUT_DEGISTIRME  0xFF    // Jetson modu degistirmek istemiyor

// ============================================================
//  DURUM YAPILARI
// ============================================================

// STM'nin uygulanmis durumu (telemetriye gonderilir)
struct VehicleState {
  int8_t   solMotor;   // -100..100  (MAX_GUC sonrasi, ~ -50..50)
  int8_t   sagMotor;   // -100..100
  uint8_t  pan;        // 0..180 derece
  uint8_t  tilt;       // 0..180 derece
  uint8_t  lazer;      // 0/1
  uint8_t  aktifMod;   // 0=surus, 1=lazer
  uint8_t  elrsLink;   // 0/1 (crsf.isLinkUp)
  uint8_t  durum;      // ST_* bitfield
};

// Jetson'dan gelen son gecerli komut (dogrulanmis)
struct JetsonKomut {
  int8_t   solHedef;   // -100..100
  int8_t   sagHedef;   // -100..100
  uint8_t  panHedef;   // 0..180 (nihai clamp .ino'da)
  uint8_t  tiltHedef;  // 0..180
  uint8_t  lazerKomut; // 0/1
  uint8_t  modKomut;   // 0/1/0xFF
  uint8_t  bayrak;     // CMD_FLAG_*
  uint32_t sonAlim;    // millis() - son gecerli COMMAND zamani
};

// Arbitrasyonda okunmak uzere disari acilir
extern JetsonKomut jetsonKomut;

// ============================================================
//  API
// ============================================================
void     haberlesmeBaslat(uint32_t baud);   // UART3 + serial baslat
void     haberlesmeAl();                     // RX pompasi (her loop cagir, non-blocking)
void     telemetriGonder(const VehicleState& s); // STATUS paketi
void     heartbeatGonder();                  // STM heartbeat

bool     komutTaze();        // COMMAND <= CMD_TIMEOUT_MS mi
bool     jetsonLinkTaze();   // Jetson paketi <= JETSON_HB_TIMEOUT_MS mi
uint16_t haberlesmeKayip();  // toplam tespit edilen paket kaybi
bool     haberlesmeCrcHata();// son CRC hata bayragini oku+temizle

// CRC yardimcisi (Jetson tarafiyla ayni; test/dogrulama icin acik)
uint16_t crc16_ccitt(const uint8_t* data, uint16_t len);

#endif // HABERLESME_H
