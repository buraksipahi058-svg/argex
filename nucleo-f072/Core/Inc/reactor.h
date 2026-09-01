/**
  ******************************************************************************
  * @file    reactor.h
  * @brief   Jsumo/Robotus "Reactor" 60V 100A cift kanalli DC motor surucu
  *          UART (MODE 3 - Serial In) surucu katmani
  *
  * Protokol (kilavuz s.5):
  *   Tek seri hat, 38400 bps, 8N1. Her komut TEK BYTE.
  *   A kanali : 1..127   (64 = STOP,   1 = tam hiz yon1, 127 = tam hiz yon2)
  *   B kanali : 128..255 (192 = STOP, 128 = tam hiz yon1, 255 = tam hiz yon2)
  *   Son gonderilen deger surucude hafizada tutulur; her dongude
  *   tekrar gondermek zorunlu degildir (biz yine de periyodik gondeririz).
  *
  * NOT: UART modunda surucuye HC06 gibi baska bir modul BAGLANMAMALIDIR.
  * NOT: Modul donanimdan bagimsizdir; sadece include F072 HAL'e cevrildi.
  ******************************************************************************
  */

#ifndef REACTOR_H
#define REACTOR_H

#include "stm32f0xx_hal.h"
#include <stdint.h>

/* Protokol sabitleri */
#define REACTOR_BAUDRATE      38400U
#define REACTOR_A_STOP        64U
#define REACTOR_B_STOP        192U
#define REACTOR_HALF_SPAN     63    /* 64 +/- 63  ve  192 +/- 63 */

typedef struct
{
    UART_HandleTypeDef *huart;  /* bu surucuye giden UART (sadece TX kullanilir) */
    int8_t  inv_a;              /* +1 veya -1 : A kanali yon tersleme */
    int8_t  inv_b;              /* +1 veya -1 : B kanali yon tersleme */
    uint8_t last_a;             /* son gonderilen A byte'i */
    uint8_t last_b;             /* son gonderilen B byte'i */
} reactor_t;

/**
  * @brief Surucu nesnesini baslatir ve motorlari durdurur.
  * @param inv_a/inv_b Motor ters donuyorsa -1 verin.
  */
void Reactor_Init(reactor_t *r, UART_HandleTypeDef *huart, int8_t inv_a, int8_t inv_b);

/**
  * @brief Iki kanala hiz komutu gonderir.
  * @param speed_a  -1000..+1000 (0 = dur)
  * @param speed_b  -1000..+1000 (0 = dur)
  */
void Reactor_SetSpeed(reactor_t *r, int16_t speed_a, int16_t speed_b);

/**
  * @brief Her iki kanali da durdurur (failsafe icin).
  */
void Reactor_Stop(reactor_t *r);

#endif /* REACTOR_H */
