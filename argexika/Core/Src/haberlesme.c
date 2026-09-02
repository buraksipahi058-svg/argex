/**
  ******************************************************************************
  * @file    haberlesme.c
  * @brief   STM32 <-> Jetson ikili protokol (HAL/C portu) — USB-CDC tasima
  *
  *  Cerceve olusturma, eski Arduino firmware'inin haberlesme.cpp dosyasindaki
  *  frameGonder / telemetriGonder / heartbeatGonder ile BIREBIR aynidir. Tasima
  *  katmani USART6'dan native USB Full-Speed CDC'ye tasindi (PA11/PA12); protokol
  *  baytlari degismedi. TX = CDC_Transmit_FS, RX = CDC_Receive_FS -> halka tampon.
  ******************************************************************************
  */

#include "haberlesme.h"
#include "main.h"          /* Error_Handler() */
#include "usbd_cdc_if.h"   /* CDC_Transmit_FS(), USBD_OK/USBD_BUSY/USBD_FAIL */

/* Giden paket sayaci (tum tipler ortak; 255 -> 0 otomatik sarar). */
static uint8_t s_seq = 0;

/* ---- RX (Jetson -> STM) durumu: SPSC halka tampon -------------------------- */
/* CDC_Receive_FS (USB IRQ) 'head'i ilerletir; Haberlesme_Poll (ana dongu) 'tail'i.
   Tek uretici + tek tuketici oldugu icin kilit gerekmez (Cortex-M4 word atomik). */
static uint8_t           s_rx_buf[HABERLESME_RX_BUF];  /* dairesel tampon */
static volatile uint16_t s_rx_head;                    /* USB IRQ yazar */
static uint16_t          s_rx_tail;                    /* ana dongu okur */

/* Cerceve cozucu durum makinesi (AA 55 | VER TYPE LEN SEQ | PAYLOAD | CRC_L H) */
enum { RX_H0, RX_H1, RX_VER, RX_TYP, RX_LEN, RX_SEQ, RX_PAY, RX_CRCL, RX_CRCH };
static uint8_t  s_rx_state;
static uint8_t  s_rx_len;                            /* payload uzunlugu */
static uint8_t  s_rx_i;                              /* payload dolum sayaci */
static uint8_t  s_rx_crc_lo;                         /* CRC dusuk bayt gecici */
static uint8_t  s_asm[4U + PROTO_MAX_PAYLOAD];       /* CRC kapsami: VER,TYPE,LEN,SEQ,payload */

/* SEQ kayip + CRC hata izleme */
static bool     s_rx_seq_valid;
static uint8_t  s_rx_last_seq;
static uint16_t s_rx_lost;
static bool     s_rx_crc_err;

/* Cozulmus son komut + Jetson link zamani */
static JetsonKomut s_komut;
static bool     s_komut_alindi;
static uint32_t s_jetson_son;
static bool     s_jetson_goruldu;

/* ============================================================
 *  CRC-16/CCITT-FALSE  (poly 0x1021, init 0xFFFF, xorout 0x0000)
 * ============================================================ */
uint16_t Haberlesme_Crc16(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < len; i++)
    {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t b = 0; b < 8; b++)
        {
            if (crc & 0x8000) { crc = (uint16_t)((crc << 1) ^ 0x1021); }
            else              { crc = (uint16_t)(crc << 1); }
        }
    }
    return crc;
}

/* ============================================================
 *  GENEL CERCEVE GONDERICI
 * ============================================================ */
static void frame_send(uint8_t type, const uint8_t *payload, uint8_t len)
{
    uint8_t buf[6 + PROTO_MAX_PAYLOAD + 2];

    buf[0] = PROTO_HDR0;
    buf[1] = PROTO_HDR1;
    buf[2] = PROTO_VERSION;
    buf[3] = type;
    buf[4] = len;
    buf[5] = s_seq++;
    for (uint8_t i = 0; i < len; i++) { buf[6 + i] = payload[i]; }

    /* CRC kapsam: VERSION..PAYLOAD  ->  buf[2] .. buf[5+len] = 4 + len bayt */
    uint16_t crc = Haberlesme_Crc16(&buf[2], (uint16_t)(4 + len));
    buf[6 + len] = (uint8_t)(crc & 0xFF);          /* low  */
    buf[7 + len] = (uint8_t)((crc >> 8) & 0xFF);   /* high */

    /* USB-CDC ile gonder. CDC_Transmit_FS onceki IN transferi bitmediyse
       USBD_BUSY doner; STATUS/HB araligi (>=50 ms) yaninda bu neredeyse hic
       olmaz, yine de kisa bir sinirla bekleriz. USB bagli/konfigure degilse
       (host yok) FAIL doner -> cerceve sessizce duser, dongu tikanmaz. */
    uint32_t t0 = HAL_GetTick();
    for (;;)
    {
        if (CDC_Transmit_FS(buf, (uint16_t)(8 + len)) != USBD_BUSY) { break; }
        if ((HAL_GetTick() - t0) >= 5U) { break; }
    }
}

/* ============================================================
 *  BASLATMA
 *  USB (CDC) main.c'de MX_USB_DEVICE_Init() ile kurulur; burada donanim
 *  kurmaya gerek yok. Yalniz protokol durumunu ve halka tamponu sifirlariz.
 * ============================================================ */
void Haberlesme_Init(void)
{
    /* TX + RX durumlarini sifirla */
    s_seq            = 0;
    s_rx_head        = 0;
    s_rx_tail        = 0;
    s_rx_state       = RX_H0;
    s_rx_seq_valid   = false;
    s_rx_last_seq    = 0;
    s_rx_lost        = 0;
    s_rx_crc_err     = false;
    s_komut_alindi   = false;
    s_jetson_son     = 0;
    s_jetson_goruldu = false;
}

/* ============================================================
 *  USB-CDC RX: gelen baytlari halka tampona koy.
 *  CDC_Receive_FS'ten (USB IRQ baglami) cagrilir; parse islemi burada DEGIL,
 *  ana donguda Haberlesme_Poll'da yapilir. Tampon dolarsa yeni bayt dusurulur
 *  (cozucu bir sonraki AA 55 basligindan resenkron olur).
 * ============================================================ */
void Haberlesme_CdcRxPush(const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++)
    {
        uint16_t next = (uint16_t)(s_rx_head + 1U);
        if (next >= HABERLESME_RX_BUF) { next = 0U; }
        if (next == s_rx_tail) { break; }      /* tampon dolu -> gerisini dusur */
        s_rx_buf[s_rx_head] = data[i];
        s_rx_head = next;
    }
}

/* ============================================================
 *  STATUS
 * ============================================================ */
void Haberlesme_SendStatus(const VehicleState *s)
{
    uint8_t p[8];
    p[0] = (uint8_t)s->solMotor;   /* int8 -> bit deseni korunur */
    p[1] = (uint8_t)s->sagMotor;
    p[2] = s->pan;
    p[3] = s->tilt;
    p[4] = s->lazer;
    p[5] = s->aktifMod;
    p[6] = s->elrsLink;
    p[7] = s->durum;
    frame_send(TYPE_STATUS, p, 8);
}

/* ============================================================
 *  HEARTBEAT (STM)
 * ============================================================ */
void Haberlesme_SendHeartbeat(uint32_t uptime_ms)
{
    uint8_t p[5];
    p[0] = HB_KAYNAK_STM;
    p[1] = (uint8_t)(uptime_ms & 0xFF);           /* uint32 LE */
    p[2] = (uint8_t)((uptime_ms >> 8) & 0xFF);
    p[3] = (uint8_t)((uptime_ms >> 16) & 0xFF);
    p[4] = (uint8_t)((uptime_ms >> 24) & 0xFF);
    frame_send(TYPE_HEARTBEAT, p, 5);
}

/* ============================================================
 *  RX: CERCEVE COZUCU  (needtocheck/jetson_parser.py::feed aynasi)
 *  Cerceve: AA 55 | VER | TYPE | LEN | SEQ | PAYLOAD | CRC_L CRC_H
 *  CRC kapsam: VER..PAYLOAD  =  s_asm[0 .. 4+LEN-1]  (2 header + CRC haric)
 * ============================================================ */

/* CRC gecerli tam bir cerceve toplandiginda cagrilir: tipe gore dagitir. */
static void rx_frame_ok(uint32_t now_ms)
{
    const uint8_t  type = s_asm[1];
    const uint8_t  len  = s_rx_len;
    const uint8_t  seq  = s_asm[3];
    const uint8_t *pl   = &s_asm[4];

    /* SEQ atlamasindan paket kaybi tahmini (0..255 sarmali) */
    if (s_rx_seq_valid)
    {
        uint8_t delta = (uint8_t)(seq - s_rx_last_seq);
        if (delta > 1U) { s_rx_lost = (uint16_t)(s_rx_lost + (delta - 1U)); }
    }
    s_rx_last_seq  = seq;
    s_rx_seq_valid = true;

    if (type == TYPE_COMMAND && len >= 7U)
    {
        s_komut.solHedef   = (int8_t)pl[0];
        s_komut.sagHedef   = (int8_t)pl[1];
        s_komut.panHedef   = pl[2];
        s_komut.tiltHedef  = pl[3];
        s_komut.lazerKomut = pl[4];
        s_komut.modKomut   = pl[5];
        s_komut.bayrak     = pl[6];
        s_komut.sonAlim    = now_ms;
        s_komut_alindi     = true;
        s_jetson_son       = now_ms;      /* COMMAND de Jetson linkini tazeler */
        s_jetson_goruldu   = true;
    }
    else if (type == TYPE_HEARTBEAT && len >= 1U)
    {
        if (pl[0] == HB_KAYNAK_JETSON)
        {
            s_jetson_son     = now_ms;
            s_jetson_goruldu = true;
        }
    }
    /* diger tipler (STATUS/IMU vs. STM'e gelmez) sessizce yok sayilir */
}

/* Tek bayti durum makinesine besler. */
static void rx_parse_byte(uint8_t b, uint32_t now_ms)
{
    switch (s_rx_state)
    {
    case RX_H0:
        if (b == PROTO_HDR0) { s_rx_state = RX_H1; }
        break;

    case RX_H1:
        if      (b == PROTO_HDR1) { s_rx_state = RX_VER; }
        else if (b == PROTO_HDR0) { s_rx_state = RX_H1; }   /* AA AA ... 55 */
        else                      { s_rx_state = RX_H0; }
        break;

    case RX_VER:
        s_asm[0]   = b;                    /* VERSION (kapsama dahil) */
        s_rx_state = RX_TYP;
        break;

    case RX_TYP:
        s_asm[1]   = b;                    /* TYPE */
        s_rx_state = RX_LEN;
        break;

    case RX_LEN:
        if (b > PROTO_MAX_PAYLOAD) { s_rx_state = RX_H0; break; }  /* sacma len -> senkron kaybi */
        s_asm[2]   = b;                    /* LENGTH */
        s_rx_len   = b;
        s_rx_state = RX_SEQ;
        break;

    case RX_SEQ:
        s_asm[3]   = b;                    /* SEQ */
        s_rx_i     = 0;
        s_rx_state = (s_rx_len > 0U) ? RX_PAY : RX_CRCL;
        break;

    case RX_PAY:
        s_asm[4U + s_rx_i] = b;
        s_rx_i++;
        if (s_rx_i >= s_rx_len) { s_rx_state = RX_CRCL; }
        break;

    case RX_CRCL:
        s_rx_crc_lo = b;
        s_rx_state  = RX_CRCH;
        break;

    case RX_CRCH:
    {
        uint16_t crc_rx   = (uint16_t)s_rx_crc_lo | ((uint16_t)b << 8);
        uint16_t crc_calc = Haberlesme_Crc16(s_asm, (uint16_t)(4U + s_rx_len));
        if (crc_calc == crc_rx) { rx_frame_ok(now_ms); }
        else                    { s_rx_crc_err = true; }
        s_rx_state = RX_H0;
        break;
    }

    default:
        s_rx_state = RX_H0;
        break;
    }
}

void Haberlesme_Poll(uint32_t now_ms)
{
    /* USB IRQ'nun (CDC_Receive_FS -> Haberlesme_CdcRxPush) doldurdugu halka
       tamponu bosalt, her bayti cozucuye besle. */
    uint16_t head = s_rx_head;               /* volatile'in anlik goruntusu */
    while (s_rx_tail != head)
    {
        rx_parse_byte(s_rx_buf[s_rx_tail], now_ms);
        s_rx_tail++;
        if (s_rx_tail >= HABERLESME_RX_BUF) { s_rx_tail = 0; }
    }
}

const JetsonKomut *Haberlesme_GetKomut(void)
{
    return &s_komut;
}

bool Haberlesme_KomutTaze(uint32_t now_ms)
{
    if (!s_komut_alindi) { return false; }
    return ((now_ms - s_komut.sonAlim) < CMD_TIMEOUT_MS);
}

bool Haberlesme_JetsonLinkTaze(uint32_t now_ms)
{
    if (!s_jetson_goruldu) { return false; }
    return ((now_ms - s_jetson_son) < JETSON_HB_TIMEOUT_MS);
}

uint16_t Haberlesme_Kayip(void)
{
    return s_rx_lost;
}

bool Haberlesme_CrcHata(void)
{
    bool v = s_rx_crc_err;
    s_rx_crc_err = false;
    return v;
}

void Haberlesme_DmaRxIrq(void)
{
    /* LEGACY: USART6 RX DMA kancasiydi. USB-CDC'ye gecince kullanilmiyor.
       stm32f4xx_it.c hala DMA2_Stream1_IRQHandler'dan cagirdigi icin sembol
       korunuyor; DMA2 Stream1 artik kurulmadigindan bu kesme hic tetiklenmez. */
}
