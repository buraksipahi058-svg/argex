/**
  ******************************************************************************
  * @file    crsf.c
  * @brief   CRSF (ExpressLRS) protokol cozucu implementasyonu
  *          Donanimdan bagimsiz -> argexika ile BIREBIR ayni.
  ******************************************************************************
  */

#include "crsf.h"

/* Parser durumlari */
#define ST_WAIT_SYNC   0
#define ST_WAIT_LEN    1
#define ST_PAYLOAD     2

/* --------------------------------------------------------------------------
 * CRC8, polinom 0x D5 (CRSF standardi). Tablosuz, dongusel hesap.
 * ------------------------------------------------------------------------ */
static uint8_t crsf_crc8(const uint8_t *p, uint8_t len)
{
    uint8_t crc = 0;
    for (uint8_t i = 0; i < len; i++)
    {
        crc ^= p[i];
        for (uint8_t j = 0; j < 8; j++)
        {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0xD5)
                               : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

/* --------------------------------------------------------------------------
 * Ham CRSF degeri (172..1811) -> mikrosaniye (988..2012)
 * us = raw * 5 / 8 + 880   (Betaflight/ELRS ile ayni donusum)
 * ------------------------------------------------------------------------ */
static uint16_t crsf_raw_to_us(uint16_t raw)
{
    return (uint16_t)(((uint32_t)raw * 5U) / 8U + 880U);
}

/* --------------------------------------------------------------------------
 * 22 byte'lik paketten 16 adet 11-bit kanali cozer (LSB-first bit akisi)
 * ------------------------------------------------------------------------ */
static void crsf_unpack_channels(crsf_t *c, const uint8_t *payload)
{
    uint32_t bits       = 0;
    uint8_t  bits_avail = 0;
    uint8_t  idx        = 0;

    for (uint8_t ch = 0; ch < CRSF_MAX_CHANNELS; ch++)
    {
        while (bits_avail < 11)
        {
            bits |= ((uint32_t)payload[idx++]) << bits_avail;
            bits_avail += 8;
        }
        uint16_t raw = (uint16_t)(bits & 0x7FFU);
        bits >>= 11;
        bits_avail -= 11;

        c->channel_us[ch] = crsf_raw_to_us(raw);
    }
}

/* ------------------------------------------------------------------------ */
void CRSF_Init(crsf_t *c)
{
    for (uint8_t i = 0; i < CRSF_MAX_CHANNELS; i++)
    {
        c->channel_us[i] = CRSF_US_MID;
    }
    c->last_frame_ms   = 0;
    c->frame_count     = 0;
    c->crc_error_count = 0;
    c->state           = ST_WAIT_SYNC;
    c->len             = 0;
    c->idx             = 0;
}

/* ------------------------------------------------------------------------ */
void CRSF_ParseByte(crsf_t *c, uint8_t b, uint32_t now_ms)
{
    switch (c->state)
    {
    case ST_WAIT_SYNC:
        if (b == CRSF_SYNC_BYTE || b == CRSF_SYNC_BYTE_ALT)
        {
            c->state = ST_WAIT_LEN;
        }
        break;

    case ST_WAIT_LEN:
        /* LEN = TYPE + PAYLOAD + CRC byte sayisi */
        if (b >= 2 && b <= (CRSF_FRAME_BUF_SIZE - 1))
        {
            c->len   = b;
            c->idx   = 0;
            c->state = ST_PAYLOAD;
        }
        else
        {
            c->state = ST_WAIT_SYNC;   /* bozuk uzunluk -> senkronu kaybet */
        }
        break;

    case ST_PAYLOAD:
        c->buf[c->idx++] = b;
        if (c->idx >= c->len)
        {
            /* buf = [TYPE][PAYLOAD...][CRC] */
            uint8_t crc_calc = crsf_crc8(c->buf, (uint8_t)(c->len - 1));
            uint8_t crc_rx   = c->buf[c->len - 1];

            if (crc_calc == crc_rx)
            {
                if (c->buf[0] == CRSF_TYPE_RC_CHANNELS && c->len >= 24)
                {
                    crsf_unpack_channels(c, &c->buf[1]);
                    c->last_frame_ms = now_ms;
                    c->frame_count++;
                }
                /* diger frame tipleri (telemetri vs.) sessizce yok sayilir */
            }
            else
            {
                c->crc_error_count++;
            }
            c->state = ST_WAIT_SYNC;
        }
        break;

    default:
        c->state = ST_WAIT_SYNC;
        break;
    }
}

/* ------------------------------------------------------------------------ */
bool CRSF_IsLinkUp(const crsf_t *c, uint32_t now_ms, uint32_t timeout_ms)
{
    if (c->frame_count == 0U)
    {
        return false;               /* hic frame gelmedi */
    }
    return ((now_ms - c->last_frame_ms) < timeout_ms);
}

/* ------------------------------------------------------------------------ */
uint16_t CRSF_GetChannelUs(const crsf_t *c, uint8_t ch_1based)
{
    if (ch_1based < 1U || ch_1based > CRSF_MAX_CHANNELS)
    {
        return CRSF_US_MID;
    }
    return c->channel_us[ch_1based - 1U];
}

/* ------------------------------------------------------------------------ */
int16_t CRSF_GetChannelNorm(const crsf_t *c, uint8_t ch_1based, uint16_t deadband_us)
{
    int32_t us = (int32_t)CRSF_GetChannelUs(c, ch_1based);
    int32_t d  = us - (int32_t)CRSF_US_MID;

    if (d > -(int32_t)deadband_us && d < (int32_t)deadband_us)
    {
        return 0;
    }

    /* olu bandi cikar, kalan araligi -1000..+1000'e olcekle */
    if (d > 0) { d -= (int32_t)deadband_us; }
    else       { d += (int32_t)deadband_us; }

    int32_t span = (int32_t)(CRSF_US_MAX - CRSF_US_MID) - (int32_t)deadband_us; /* ~512-db */
    if (span < 1) { span = 1; }

    int32_t n = (d * 1000) / span;
    if (n >  1000) { n =  1000; }
    if (n < -1000) { n = -1000; }

    return (int16_t)n;
}
