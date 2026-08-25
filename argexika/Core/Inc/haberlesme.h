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
  *  Bu firmware TEK YONLU telemetri gonderir (STM -> Jetson); komut ALMAZ.
  *  Bu yuzden durum bitlerinden yalnizca ST_FAILSAFE uretilir; digerleri 0.
  *
  *  DONANIM: bos olan USART6 kullanilir. TX = PC6 (AF8) @115200 8N1.
  *           Sadece PC6 -> Jetson RX + GND kablolanir (RX kullanilmaz).
  ******************************************************************************
  */

#ifndef HABERLESME_H
#define HABERLESME_H

#include "stm32f4xx_hal.h"
#include <stdint.h>

/* ---- Cerceve sabitleri ---------------------------------------------------- */
#define PROTO_HDR0          0xAAU
#define PROTO_HDR1          0x55U
#define PROTO_VERSION       0x01U
#define PROTO_MAX_PAYLOAD   32U

/* ---- Paket tipleri -------------------------------------------------------- */
#define TYPE_STATUS         0x01U   /* STM -> Jetson */
#define TYPE_COMMAND        0x02U   /* Jetson -> STM (bu firmware kullanmaz) */
#define TYPE_HEARTBEAT      0x03U   /* cift yonlu */

/* ---- Heartbeat kaynak alani ----------------------------------------------- */
#define HB_KAYNAK_STM       0x00U
#define HB_KAYNAK_JETSON    0x01U

/* ---- Zamanlama (ms) ------------------------------------------------------- */
#define STATUS_PERIOD_MS    50U     /* 20 Hz telemetri */
#define HB_PERIOD_MS        100U    /* 10 Hz heartbeat */

/* ---- STATUS durum bayragi bitleri (payload byte 7) ------------------------ */
#define ST_JETSON_LINK      0x01U   /* Jetson linki taze  (bu firmware: 0) */
#define ST_CMD_TIMEOUT      0x02U   /* otonom komut bayat (bu firmware: 0) */
#define ST_AUTO_EN          0x04U   /* otonom kontrol aktif (bu firmware: 0) */
#define ST_FAILSAFE         0x08U   /* guvenlik motorlari durdurdu */
#define ST_CRC_ERR          0x10U   /* yakin zamanda CRC hatasi (bu firmware: 0) */

/* ---- Telemetri UART ayarlari ---------------------------------------------- */
#define HABERLESME_BAUD     115200U

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

/**
  * @brief USART6 (PC6, TX-only) donanimini kurar ve SEQ sayacini sifirlar.
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

#endif /* HABERLESME_H */
