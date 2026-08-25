/**
  ******************************************************************************
  * @file    haberlesme.c
  * @brief   STM32 -> Jetson ikili UART telemetri protokolu (HAL/C portu)
  *
  *  Cerceve olusturma, eski Arduino firmware'inin haberlesme.cpp dosyasindaki
  *  frameGonder / telemetriGonder / heartbeatGonder ile BIREBIR aynidir; ama
  *  Arduino/HardwareSerial yerine STM32 HAL (USART6, blocking TX) kullanir.
  ******************************************************************************
  */

#include "haberlesme.h"
#include "main.h"        /* Error_Handler() */

/* USART6, yalnizca TX. Modul icine kapali. */
static UART_HandleTypeDef s_huart;

/* Giden paket sayaci (tum tipler ortak; 255 -> 0 otomatik sarar). */
static uint8_t s_seq = 0;

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

    /* 115200'de en buyuk cerceve (STATUS) 16 bayt ~= 1.4 ms; 100 Hz donguyu
       tikamaz. Reactor surucusuyle ayni blocking TX yaklasimi. */
    HAL_UART_Transmit(&s_huart, buf, (uint16_t)(8 + len), 10);
}

/* ============================================================
 *  BASLATMA  (USART6 / PC6, TX-only)
 *  Not: HAL_UART_MspInit USART6 bilmiyor; bu yuzden clock + GPIO'yu
 *  HAL_UART_Init'ten ONCE burada elle kuruyoruz (modul kendi kendine yeter,
 *  main.c / stm32f4xx_hal_msp.c'ye dokunmaya gerek kalmaz).
 * ============================================================ */
void Haberlesme_Init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_USART6_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    /* PC6 -> USART6_TX (AF8) */
    gpio.Pin       = GPIO_PIN_6;
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_NOPULL;
    gpio.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF8_USART6;
    HAL_GPIO_Init(GPIOC, &gpio);

    s_huart.Instance          = USART6;
    s_huart.Init.BaudRate     = HABERLESME_BAUD;
    s_huart.Init.WordLength   = UART_WORDLENGTH_8B;
    s_huart.Init.StopBits     = UART_STOPBITS_1;
    s_huart.Init.Parity       = UART_PARITY_NONE;
    s_huart.Init.Mode         = UART_MODE_TX;
    s_huart.Init.HwFlowCtl    = UART_HWCONTROL_NONE;
    s_huart.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(&s_huart) != HAL_OK)
    {
        Error_Handler();
    }

    s_seq = 0;
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
