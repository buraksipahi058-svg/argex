/**
  ******************************************************************************
  * @file    haberlesme.h
  * @brief   STM32 -> Jetson ikili UART telemetri protokolu (HAL/C portu)
  *
  *  Cerceve:  AA 55 | VERSION | TYPE | LENGTH | SEQ | PAYLOAD | CRC_L CRC_H
  *   - Cok baytli alanlar LITTLE-ENDIAN
  *   - CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF), kapsam: VERSION..PAYLOAD
  *
  *  Base station referansi (TEK GERCEK KAYNAK): needtocheck/jetson_parser.py
  *  Uretilen cerceveler o parser + sim/fake_stm.py ile BIREBIR ayni olmalidir.
  *
  *  Bu firmware CIFT YONLUDUR:
  *    - TX (STM -> Jetson): STATUS + HEARTBEAT telemetri
  *    - RX (Jetson -> STM): COMMAND + HEARTBEAT  (halka tampon + cozucu)
  *  Gelen COMMAND dogrulanip JetsonKomut'a yazilir; bu SURUMDE surus/servo
  *  kontroluna BAGLANMAZ (yalniz alinir ve tazeligi izlenir). Motora uygulama
  *  (RC <-> AI arbitrasyonu) sonraki adimda main.c::Drive_Update'te yapilacak.
  *
  *  DONANIM: native USB Full-Speed CDC (Virtual COM Port), OTG_FS.
  *           PA11 = USB_DM , PA12 = USB_DP.  Jetson'da /dev/ttyACM* olarak gorunur.
  *           TX -> CDC_Transmit_FS ; RX -> CDC_Receive_FS (USB IRQ) gelen baytlari
  *           halka tampona yazar, Haberlesme_Poll() ana donguda bosaltir.
  *           (Onceki surum: USART6 PC6/PC7 + DMA2 Stream1.)
  ******************************************************************************
  */

#ifndef HABERLESME_H
#define HABERLESME_H

#include "stm32f4xx_hal.h"
#include <stdint.h>
#include <stdbool.h>

/* ---- Cerceve sabitleri ---------------------------------------------------- */
#define PROTO_HDR0          0xAAU
#define PROTO_HDR1          0x55U
#define PROTO_VERSION       0x01U
#define PROTO_MAX_PAYLOAD   32U

/* ---- Paket tipleri -------------------------------------------------------- */
#define TYPE_STATUS         0x01U   /* STM -> Jetson */
#define TYPE_COMMAND        0x02U   /* Jetson -> STM (bu firmware ARTIK cozer)  */
#define TYPE_HEARTBEAT      0x03U   /* cift yonlu */
/* 0x04 ACK, 0x05 CONFIG, 0x06 LOG rezerve; 0x07 = IMU (sonraki adim) */

/* ---- Heartbeat kaynak alani ----------------------------------------------- */
#define HB_KAYNAK_STM       0x00U
#define HB_KAYNAK_JETSON    0x01U

/* ---- COMMAND bayrak bitleri (COMMAND payload byte 6) ---------------------- */
#define CMD_FLAG_AUTO_REQ   0x01U   /* Jetson otonom kontrol istiyor */

/* ---- Mod komutu ozel degeri ----------------------------------------------- */
#define MOD_KOMUT_DEGISTIRME 0xFFU  /* Jetson modu degistirmek istemiyor */

/* ---- Zamanlama (ms) ------------------------------------------------------- */
#define STATUS_PERIOD_MS    50U     /* 20 Hz telemetri */
#define HB_PERIOD_MS        100U    /* 10 Hz heartbeat */
#define CMD_TIMEOUT_MS      200U    /* COMMAND bu sureden eskiyse bayat */
#define JETSON_HB_TIMEOUT_MS 500U   /* Jetson linki bu sureden sessizse kopuk */

/* ---- STATUS durum bayragi bitleri (payload byte 7) ------------------------ */
#define ST_JETSON_LINK      0x01U   /* Jetson linki taze  (bu firmware: 0) */
#define ST_CMD_TIMEOUT      0x02U   /* otonom komut bayat (bu firmware: 0) */
#define ST_AUTO_EN          0x04U   /* otonom kontrol aktif (bu firmware: 0) */
#define ST_FAILSAFE         0x08U   /* guvenlik motorlari durdurdu */
#define ST_CRC_ERR          0x10U   /* yakin zamanda CRC hatasi (bu firmware: 0) */

/* ---- Telemetri UART ayarlari ---------------------------------------------- */
#define HABERLESME_BAUD     115200U
#define HABERLESME_RX_BUF   256U    /* Jetson->STM DMA dairesel tampon boyutu */

/* STM'nin uygulanmis durumu (telemetriye gonderilir) */
typedef struct
{
    int8_t   solMotor;   /* -100..100 (uygulanan sol palet hizi) */
    int8_t   sagMotor;   /* -100..100 (uygulanan sag palet hizi) */
    uint8_t  pan;        /* 0..180 derece */
    uint8_t  tilt;       /* 0..180 derece */
    uint8_t  lazer;      /* 0/1 */
    uint8_t  aktifMod;   /* 0=surus */
    uint8_t  elrsLink;   /* 0/1 (CRSF link durumu) */
    uint8_t  durum;      /* ST_* bitfield */
} VehicleState;

/* Jetson'dan gelen son dogrulanmis COMMAND. Surus katmanina ACIK; bu surumde
   yalnizca doldurulur, motorlara/servolara UYGULANMAZ (arbitrasyon sonraki adim). */
typedef struct
{
    int8_t   solHedef;   /* -100..100 */
    int8_t   sagHedef;   /* -100..100 */
    uint8_t  panHedef;   /* 0..180 */
    uint8_t  tiltHedef;  /* 0..180 */
    uint8_t  lazerKomut; /* 0/1 */
    uint8_t  modKomut;   /* 0/1 veya MOD_KOMUT_DEGISTIRME (0xFF) */
    uint8_t  bayrak;     /* CMD_FLAG_* */
    uint32_t sonAlim;    /* son gecerli COMMAND zamani (HAL_GetTick) */
} JetsonKomut;

/**
  * @brief Protokol durum makinesini/sayaclarini ve RX halka tamponunu sifirlar.
  *        USB (CDC) main icinde MX_USB_DEVICE_Init() ile ayrica baslatilir;
  *        bu fonksiyon donanim kurmaz (USB-CDC'de gerekmez).
  */
void Haberlesme_Init(void);

/**
  * @brief STATUS paketi gonderir (20 Hz cagirilmali).
  */
void Haberlesme_SendStatus(const VehicleState *s);

/**
  * @brief STM HEARTBEAT paketi gonderir (10 Hz cagirilmali).
  * @param uptime_ms  Acilistan bu yana gecen ms (HAL_GetTick()).
  */
void Haberlesme_SendHeartbeat(uint32_t uptime_ms);

/**
  * @brief CRC-16/CCITT-FALSE (Jetson tarafiyla ayni; test icin acik).
  */
uint16_t Haberlesme_Crc16(const uint8_t *data, uint16_t len);

/* ==========================================================================
 *  RX yolu (Jetson -> STM):  COMMAND + HEARTBEAT cozucu
 * ========================================================================== */

/**
  * @brief USART6 RX DMA dairesel tamponunu bosaltip gelen cerceveleri cozer.
  *        Ana dongude her spin (CRSF_PumpDMA gibi, non-blocking) cagirin.
  * @param now_ms  HAL_GetTick() — tazelik ve paket-kaybi zaman damgalari icin.
  */
void Haberlesme_Poll(uint32_t now_ms);

/**
  * @brief Son dogrulanmis Jetson komutuna salt-okunur erisim.
  */
const JetsonKomut *Haberlesme_GetKomut(void);

/**
  * @brief COMMAND <= CMD_TIMEOUT_MS icinde mi geldi (taze mi).
  */
bool Haberlesme_KomutTaze(uint32_t now_ms);

/**
  * @brief Jetson paketi (COMMAND/HEARTBEAT) <= JETSON_HB_TIMEOUT_MS icinde mi.
  */
bool Haberlesme_JetsonLinkTaze(uint32_t now_ms);

/**
  * @brief SEQ atlamalarindan tespit edilen toplam paket kaybi.
  */
uint16_t Haberlesme_Kayip(void);

/**
  * @brief Son CRC hata bayragini okuyup temizler.
  */
bool Haberlesme_CrcHata(void);

/**
  * @brief CDC_Receive_FS (USB IRQ) tarafindan cagrilir: gelen USB baytlarini
  *        RX halka tamponuna kopyalar (parse ana donguda Haberlesme_Poll'da).
  */
void Haberlesme_CdcRxPush(const uint8_t *data, uint32_t len);

/**
  * @brief (LEGACY) USART6 RX DMA kesme kancasi. USB-CDC'ye gecince kullanilmiyor;
  *        stm32f4xx_it.c hala cagirdigi icin sembol korunur (no-op).
  */
void Haberlesme_DmaRxIrq(void);

#endif /* HABERLESME_H */
