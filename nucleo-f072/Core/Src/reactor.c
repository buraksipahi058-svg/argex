/**
  ******************************************************************************
  * @file    reactor.c
  * @brief   Reactor motor surucu UART (MODE 3) surucu katmani
  *          Donanimdan bagimsiz -> argexika ile BIREBIR ayni.
  ******************************************************************************
  */

#include "reactor.h"

/* --------------------------------------------------------------------------
 * -1000..+1000 -> A kanali byte'i (1..127, merkez 64)
 * ------------------------------------------------------------------------ */
static uint8_t reactor_byte_a(int16_t speed)
{
    int32_t v = (int32_t)REACTOR_A_STOP + ((int32_t)speed * REACTOR_HALF_SPAN) / 1000;
    if (v < 1)   { v = 1;   }
    if (v > 127) { v = 127; }
    return (uint8_t)v;
}

/* --------------------------------------------------------------------------
 * -1000..+1000 -> B kanali byte'i (128..255, merkez 192)
 * ------------------------------------------------------------------------ */
static uint8_t reactor_byte_b(int16_t speed)
{
    int32_t v = (int32_t)REACTOR_B_STOP + ((int32_t)speed * REACTOR_HALF_SPAN) / 1000;
    if (v < 128) { v = 128; }
    if (v > 255) { v = 255; }
    return (uint8_t)v;
}

/* ------------------------------------------------------------------------ */
void Reactor_Init(reactor_t *r, UART_HandleTypeDef *huart, int8_t inv_a, int8_t inv_b)
{
    r->huart  = huart;
    r->inv_a  = (inv_a < 0) ? -1 : 1;
    r->inv_b  = (inv_b < 0) ? -1 : 1;
    r->last_a = REACTOR_A_STOP;
    r->last_b = REACTOR_B_STOP;
}

/* ------------------------------------------------------------------------ */
void Reactor_SetSpeed(reactor_t *r, int16_t speed_a, int16_t speed_b)
{
    if (r->huart == NULL) { return; }

    if (speed_a >  1000) { speed_a =  1000; }
    if (speed_a < -1000) { speed_a = -1000; }
    if (speed_b >  1000) { speed_b =  1000; }
    if (speed_b < -1000) { speed_b = -1000; }

    uint8_t tx[2];
    tx[0] = reactor_byte_a((int16_t)(speed_a * r->inv_a));
    tx[1] = reactor_byte_b((int16_t)(speed_b * r->inv_b));

    r->last_a = tx[0];
    r->last_b = tx[1];

    /* 38400 bps'de 2 byte ~= 0.52 ms; 100 Hz dongude sorun cikarmaz. */
    HAL_UART_Transmit(r->huart, tx, 2, 5);
}

/* ------------------------------------------------------------------------ */
void Reactor_Stop(reactor_t *r)
{
    if (r->huart == NULL) { return; }

    uint8_t tx[2];
    tx[0] = REACTOR_A_STOP;
    tx[1] = REACTOR_B_STOP;

    r->last_a = tx[0];
    r->last_b = tx[1];

    HAL_UART_Transmit(r->huart, tx, 2, 5);
}
