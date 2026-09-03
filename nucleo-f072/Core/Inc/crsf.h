/**
  ******************************************************************************
  * @file    crsf.h
  * @brief   CRSF (Crossfire / ExpressLRS) RC kanal cozucu
  *
  * ELRS alicisinin TX pedinden cikan seri veriyi cozer.
  * Baudrate: 420000, 8N1
  * Frame:  [0xC8][LEN][TYPE][PAYLOAD...][CRC8(poly 0xD5)]
  * TYPE 0x16 = RC_CHANNELS_PACKED -> 22 byte icinde 16 adet 11-bit kanal
  ******************************************************************************
  */

#ifndef CRSF_H
#define CRSF_H

#include <stdint.h>
#include <stdbool.h>

#define CRSF_BAUDRATE            420000U
#define CRSF_MAX_CHANNELS        16
#define CRSF_SYNC_BYTE           0xC8
#define CRSF_SYNC_BYTE_ALT       0xEE   /* bazi aliciler bu adresi kullanir */
#define CRSF_TYPE_RC_CHANNELS    0x16
#define CRSF_FRAME_BUF_SIZE      64

/* Kanal degerlerinin mikrosaniye karsiliklari */
#define CRSF_US_MIN              988U
#define CRSF_US_MID              1500U
#define CRSF_US_MAX              2012U

typedef struct
{
    /* --- disaridan okunabilir --- */
    uint16_t channel_us[CRSF_MAX_CHANNELS]; /* 988..2012 us */
    uint32_t last_frame_ms;                 /* son gecerli frame zamani */
    uint32_t frame_count;                   /* gecerli frame sayaci */
    uint32_t crc_error_count;               /* hatali frame sayaci */

    /* --- dahili parser durumu --- */
    uint8_t  state;
    uint8_t  len;
    uint8_t  idx;
    uint8_t  buf[CRSF_FRAME_BUF_SIZE];
} crsf_t;

/**
  * @brief Parser'i sifirlar, tum kanallari 1500us'e cekar.
  */
void CRSF_Init(crsf_t *c);

/**
  * @brief UART'tan gelen tek bir byte'i parser'a besler.
  * @param now_ms HAL_GetTick() degeri
  */
void CRSF_ParseByte(crsf_t *c, uint8_t b, uint32_t now_ms);

/**
  * @brief Baglanti canli mi? (timeout_ms icinde gecerli frame geldi mi)
  */
bool CRSF_IsLinkUp(const crsf_t *c, uint32_t now_ms, uint32_t timeout_ms);

/**
  * @brief Kanal degerini mikrosaniye olarak dondurur.
  * @param ch_1based 1..16 (kumandadaki CH1..CH16 ile birebir)
  * @retval 988..2012 us, gecersiz kanalda 1500
  */
uint16_t CRSF_GetChannelUs(const crsf_t *c, uint8_t ch_1based);

/**
  * @brief Kanali -1000..+1000 araligina normalize eder (orta = 0).
  * @param deadband_us Merkez olu bant (us). Tipik 20-30.
  */
int16_t CRSF_GetChannelNorm(const crsf_t *c, uint8_t ch_1based, uint16_t deadband_us);

#endif /* CRSF_H */
