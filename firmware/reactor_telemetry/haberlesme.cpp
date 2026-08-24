#include "haberlesme.h"
#include <HardwareSerial.h>

// ============================================================
//  JETSON UART PIN / BAUD
//  >>> BOARD PIN HARITASI TEYIT EDILINCE SADECE BURAYI DEGISTIR <<<
//  [REACTOR ENTEGRASYON] Reactor firmware'inde USART3(PB10/11)=CRSF,
//  USART2(PA2)=sol motor, USART6(PC6)=sag motor dolu. Bu yuzden Jetson
//  telemetrisi bos olan USART1'e alindi: TX=PB6, RX=PB7.
//  Tek yonlu telemetri: sadece PB6 -> Jetson RX kablolanir (PB7 kullanilmaz).
//  Protokolun geri kalani (cerceve/CRC/STATUS/HEARTBEAT) DEGISMEDI.
// ============================================================
#define JETSON_TX_PIN   PB6
#define JETSON_RX_PIN   PB7

HardwareSerial jetsonSerial(JETSON_RX_PIN, JETSON_TX_PIN);  // (RX, TX)

// ---- Global durum (bu modul icine kapali) ----------------------------------
JetsonKomut jetsonKomut = {0, 0, 90, 90, 0, MOD_KOMUT_DEGISTIRME, 0, 0};

static uint8_t  g_txSeq       = 0;      // giden paket sayaci (tum tipler ortak)
static uint8_t  g_rxSonSeq    = 0;      // gelenin son SEQ'i
static bool     g_rxSeqIlk    = true;   // ilk paket mi
static uint16_t g_kayip       = 0;      // tespit edilen kayip
static uint32_t g_sonJetsonHB = 0;      // son GECERLI frame zamani (herhangi tip)
static volatile bool g_crcHata = false; // CRC hata bayragi

// ============================================================
//  CRC-16/CCITT-FALSE
// ============================================================
uint16_t crc16_ccitt(const uint8_t* data, uint16_t len) {
  uint16_t crc = 0xFFFF;
  for (uint16_t i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i] << 8;
    for (uint8_t b = 0; b < 8; b++) {
      if (crc & 0x8000) crc = (crc << 1) ^ 0x1021;
      else              crc = (crc << 1);
    }
  }
  return crc;
}

// ============================================================
//  BASLATMA
// ============================================================
void haberlesmeBaslat(uint32_t baud) {
  jetsonSerial.begin(baud, SERIAL_8N1);
  g_sonJetsonHB = 0;          // baslangicta link YOK say
  jetsonKomut.sonAlim = 0;    // komut YOK say
}

// ============================================================
//  GENEL FRAME GONDERICI
// ============================================================
static void frameGonder(uint8_t type, const uint8_t* payload, uint8_t len) {
  uint8_t buf[6 + PROTO_MAX_PAYLOAD + 2];
  buf[0] = PROTO_HDR0;
  buf[1] = PROTO_HDR1;
  buf[2] = PROTO_VERSION;
  buf[3] = type;
  buf[4] = len;
  buf[5] = g_txSeq++;                 // 255 -> 0 otomatik sarar
  for (uint8_t i = 0; i < len; i++) buf[6 + i] = payload[i];

  uint16_t crc = crc16_ccitt(&buf[2], (uint16_t)(4 + len)); // VERSION..PAYLOAD
  buf[6 + len] = (uint8_t)(crc & 0xFF);        // low
  buf[7 + len] = (uint8_t)((crc >> 8) & 0xFF); // high

  jetsonSerial.write(buf, 8 + len);
}

// ============================================================
//  TELEMETRI (STATUS)
// ============================================================
void telemetriGonder(const VehicleState& s) {
  uint8_t p[8];
  p[0] = (uint8_t)s.solMotor;   // int8 -> bit deseni korunur
  p[1] = (uint8_t)s.sagMotor;
  p[2] = s.pan;
  p[3] = s.tilt;
  p[4] = s.lazer;
  p[5] = s.aktifMod;
  p[6] = s.elrsLink;
  p[7] = s.durum;
  frameGonder(TYPE_STATUS, p, 8);
}

// ============================================================
//  HEARTBEAT (STM)
// ============================================================
void heartbeatGonder() {
  uint8_t p[5];
  uint32_t up = millis();
  p[0] = HB_KAYNAK_STM;
  p[1] = (uint8_t)(up & 0xFF);           // uint32 LE
  p[2] = (uint8_t)((up >> 8) & 0xFF);
  p[3] = (uint8_t)((up >> 16) & 0xFF);
  p[4] = (uint8_t)((up >> 24) & 0xFF);
  frameGonder(TYPE_HEARTBEAT, p, 5);
}

// ============================================================
//  COMMAND PAYLOAD DOGRULAMA + KABUL
//  Range check burada; nihai servo clamp'i .ino otonomLazer'da.
// ============================================================
static void komutIsle(const uint8_t* pay, uint8_t len) {
  if (len < 7) return;   // v1 COMMAND en az 7 bayt; eksikse REDDET

  int8_t  sol  = (int8_t)pay[0];
  int8_t  sag  = (int8_t)pay[1];
  uint8_t pan  = pay[2];
  uint8_t tilt = pay[3];
  uint8_t laz  = pay[4];
  uint8_t mod  = pay[5];
  uint8_t byr  = pay[6];

  // RANGE CHECK
  if (sol < -100) sol = -100; else if (sol > 100) sol = 100;
  if (sag < -100) sag = -100; else if (sag > 100) sag = 100;
  if (pan  > 180) pan  = 180;      // kaba sinir; nihai 30-150 .ino'da
  if (tilt > 180) tilt = 180;
  laz = (laz != 0) ? 1 : 0;
  if (mod != 0 && mod != 1) mod = MOD_KOMUT_DEGISTIRME; // gecersiz -> degistirme

  jetsonKomut.solHedef   = sol;
  jetsonKomut.sagHedef   = sag;
  jetsonKomut.panHedef   = pan;
  jetsonKomut.tiltHedef  = tilt;
  jetsonKomut.lazerKomut = laz;
  jetsonKomut.modKomut   = mod;
  jetsonKomut.bayrak     = byr;
  jetsonKomut.sonAlim    = millis();   // yalnizca GECERLI komutta tazele
}

// ============================================================
//  FRAME ISLE (CRC dogru sonrasi)
// ============================================================
static void frameIsle(uint8_t ver, uint8_t type, uint8_t len,
                       uint8_t seq, const uint8_t* pay) {
  (void)ver;  // v1: tum versiyonlar kabul, bilinen alanlar okunur (ileri uyum)

  // SEQ kayip takibi (CRC gecmis paketler icin)
  if (g_rxSeqIlk) {
    g_rxSeqIlk = false;
  } else {
    uint8_t delta = (uint8_t)(seq - g_rxSonSeq);
    if (delta > 1) g_kayip += (uint16_t)(delta - 1);
  }
  g_rxSonSeq = seq;

  // Herhangi gecerli frame -> Jetson canli
  g_sonJetsonHB = millis();

  switch (type) {
    case TYPE_COMMAND:   komutIsle(pay, len); break;
    case TYPE_HEARTBEAT: /* zaten g_sonJetsonHB tazelendi */ break;
    default: break; // bilinmeyen tip: sessizce yok say
  }
}

// ============================================================
//  RX STATE MACHINE (non-blocking)
// ============================================================
static enum {
  RX_H0, RX_H1, RX_HDR, RX_PAY, RX_CRCL, RX_CRCH
} g_rxState = RX_H0;

static uint8_t  g_hdr[4];     // VER,TYPE,LEN,SEQ
static uint8_t  g_pay[PROTO_MAX_PAYLOAD];
static uint8_t  g_idx  = 0;
static uint8_t  g_ver, g_type, g_len, g_seq;
static uint16_t g_crcRx = 0;

void haberlesmeAl() {
  while (jetsonSerial.available()) {
    uint8_t b = (uint8_t)jetsonSerial.read();

    switch (g_rxState) {
      case RX_H0:
        if (b == PROTO_HDR0) g_rxState = RX_H1;
        break;

      case RX_H1:
        if (b == PROTO_HDR1)      { g_rxState = RX_HDR; g_idx = 0; }
        else if (b == PROTO_HDR0) { g_rxState = RX_H1; }   // AA AA .. tut
        else                      { g_rxState = RX_H0; }
        break;

      case RX_HDR:
        g_hdr[g_idx++] = b;
        if (g_idx == 4) {
          g_ver  = g_hdr[0];
          g_type = g_hdr[1];
          g_len  = g_hdr[2];
          g_seq  = g_hdr[3];
          if (g_len > PROTO_MAX_PAYLOAD) { g_rxState = RX_H0; break; } // sacma len
          g_idx = 0;
          g_rxState = (g_len == 0) ? RX_CRCL : RX_PAY;
        }
        break;

      case RX_PAY:
        g_pay[g_idx++] = b;
        if (g_idx == g_len) g_rxState = RX_CRCL;
        break;

      case RX_CRCL:
        g_crcRx = b;
        g_rxState = RX_CRCH;
        break;

      case RX_CRCH:
        g_crcRx |= ((uint16_t)b << 8);
        {
          // CRC yeniden hesapla: VERSION..PAYLOAD
          uint8_t tmp[4 + PROTO_MAX_PAYLOAD];
          tmp[0] = g_ver; tmp[1] = g_type; tmp[2] = g_len; tmp[3] = g_seq;
          for (uint8_t i = 0; i < g_len; i++) tmp[4 + i] = g_pay[i];
          uint16_t hes = crc16_ccitt(tmp, (uint16_t)(4 + g_len));
          if (hes == g_crcRx) {
            frameIsle(g_ver, g_type, g_len, g_seq, g_pay);
          } else {
            g_crcHata = true;   // paketi TAMAMEN reddet
          }
        }
        g_rxState = RX_H0;
        break;
    }
  }
}

// ============================================================
//  DURUM SORGULARI
// ============================================================
bool komutTaze() {
  if (jetsonKomut.sonAlim == 0) return false;
  return (millis() - jetsonKomut.sonAlim) <= CMD_TIMEOUT_MS;
}

bool jetsonLinkTaze() {
  if (g_sonJetsonHB == 0) return false;
  return (millis() - g_sonJetsonHB) <= JETSON_HB_TIMEOUT_MS;
}

uint16_t haberlesmeKayip() { return g_kayip; }

bool haberlesmeCrcHata() {
  bool v = g_crcHata;
  g_crcHata = false;
  return v;
}
