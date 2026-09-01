/**
  ******************************************************************************
  * @file    haberlesme.h
  * @brief   STM32 <-> Jetson ikili UART protokolu (HAL/C - F072 portu)
  *
  *  Cerceve:  AA 55 | VERSION | TYPE | LENGTH | SEQ | PAYLOAD | CRC_L CRC_H
  *   - Cok baytli alanlar LITTLE-ENDIAN
  *   - CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF), kapsam: VERSION..PAYLOAD
  *
  *  Base station referansi (TEK GERCEK KAYNAK): needtocheck/jetson_parser.py
  *  Uretilen cerceveler o parser + sim/fake_stm.py ile BIREBIR ayni olmalidir.
  *  (Protokol degismedi; sadece altindaki UART/DMA donanimi F072'ye tasindi.)
  *
  *  DONANIM (F072/Nucleo):
  *    USART2 full-duplex @115200 8N1  <-> Nucleo ST-Link Virtual COM Port (VCP)
  *      TX = PA2 (AF1) ,  RX = PA3 (AF1)  (Nucleo'da SB ile ST-Link'e bagli)
  *    Jetson, Nucleo'nun ST-Link USB'sini gorur -> /dev/ttyACM* (USB-CDC).
  *    RX yolu: DMA1 Channel5 (USART2_RX), dairesel.
  *    IRQ: DMA1_Channel4_5_6_7_IRQn -> Haberlesme_DmaRxIrq().
  ******************************************************************************
  */

#ifndef HABERLESME_H
#define HABERLESME_H

#include "stm32f0xx_hal.h"
#include <stdint.h>
#include <stdbool.h>

/* ---- Cerceve sabitleri ---------------------------------------------------- */
#define PROTO_HDR0          0xAAU
#define PROTO_HDR1          0x55U
#define PROTO_VERSION       0x01U
#define PROTO_MAX_PAYLOAD   32U

/* ---- Paket tipleri -------------------------------------------------------- */
#define TYPE_STATUS         0x01U   /* STM -> Jetson */
#define TYPE_COMMAND        0x02U   /* Jetson -> STM (bu firmware cozer) */
#define TYPE_HEARTBEAT      0x03U   /* cift yonlu */

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
#define ST_JETSON_LINK      0x01U
#define ST_CMD_TIMEOUT      0x02U
#define ST_AUTO_EN          0x04U
#define ST_FAILSAFE         0x08U
#define ST_CRC_ERR          0x10U

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
    uint8_t  aktifMod;   /* 0=surus, 1=lazer */
    uint8_t  elrsLink;   /* 0/1 (CRSF link durumu) */
    uint8_t  durum;      /* ST_* bitfield */
} VehicleState;

/* Jetson'dan gelen son dogrulanmis COMMAND. */
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
  * @brief USART2 full-duplex (PA2 TX / PA3 RX) donanimini kurar, RX DMA'yi
  *        (DMA1 Ch5) baslatir ve tum sayaclari/durum makinesini sifirlar.
  */
void Haberlesme_Init(void);

/** @brief STATUS paketi gonderir (20 Hz cagirilmali). */
void Haberlesme_SendStatus(const VehicleState *s);

/** @brief STM HEARTBEAT paketi gonderir (10 Hz cagirilmali). */
void Haberlesme_SendHeartbeat(uint32_t uptime_ms);

/** @brief CRC-16/CCITT-FALSE (Jetson tarafiyla ayni; test icin acik). */
uint16_t Haberlesme_Crc16(const uint8_t *data, uint16_t len);

/**
  * @brief USART2 RX DMA dairesel tamponunu bosaltip gelen cerceveleri cozer.
  *        Ana dongude her spin (CRSF_PumpDMA gibi, non-blocking) cagirin.
  */
void Haberlesme_Poll(uint32_t now_ms);

/** @brief Son dogrulanmis Jetson komutuna salt-okunur erisim. */
const JetsonKomut *Haberlesme_GetKomut(void);

/** @brief COMMAND <= CMD_TIMEOUT_MS icinde mi geldi (taze mi). */
bool Haberlesme_KomutTaze(uint32_t now_ms);

/** @brief Jetson paketi (COMMAND/HEARTBEAT) <= JETSON_HB_TIMEOUT_MS icinde mi. */
bool Haberlesme_JetsonLinkTaze(uint32_t now_ms);

/** @brief SEQ atlamalarindan tespit edilen toplam paket kaybi. */
uint16_t Haberlesme_Kayip(void);

/** @brief Son CRC hata bayragini okuyup temizler. */
bool Haberlesme_CrcHata(void);

/**
  * @brief DMA1 Channel5 (USART2 RX) kesme kancasi.
  *        stm32f0xx_it.c icindeki DMA1_Channel4_5_6_7_IRQHandler'dan cagrilir.
  */
void Haberlesme_DmaRxIrq(void);

#endif /* HABERLESME_H */
