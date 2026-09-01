/**
  ******************************************************************************
  * @file    servo.h
  * @brief   RDS3235 (270 derece, dijital) servo PWM surucu katmani
  *
  * Timer 1 MHz'e ayarlanir, ARR = 20000-1 -> 50 Hz.
  * Bu durumda CCR degeri dogrudan mikrosaniye demektir.
  * RDS3235 darbe araligi: 500 us .. 2500 us  (~270 derece)
  *
  * NOT (F072): Timer 1 MHz tabani icin PSC main.c'de F072'ye gore ayarlanir
  *             (TIM3 @48 MHz -> PSC = 48-1). Modul mantigi degismez.
  ******************************************************************************
  */

#ifndef SERVO_H
#define SERVO_H

#include "stm32f0xx_hal.h"
#include <stdint.h>

/* Servo fiziksel darbe siniri - bunlarin disina asla cikilmaz */
#define SERVO_ABS_MIN_US   500U
#define SERVO_ABS_MAX_US   2500U
#define SERVO_MID_US       1500U

typedef struct
{
    TIM_HandleTypeDef *htim;
    uint32_t           channel;   /* TIM_CHANNEL_1 ... */
    uint16_t           min_us;    /* mekanik guvenli alt sinir */
    uint16_t           max_us;    /* mekanik guvenli ust sinir */
    uint16_t           pos_us;    /* guncel konum */
} servo_t;

/**
  * @brief Servoyu baslatir, PWM'i acar ve orta konuma gonderir.
  * @param min_us/max_us Mekanik olarak guvenli darbe sinirlari.
  * @param start_us      Acilistaki konum.
  */
void Servo_Init(servo_t *s, TIM_HandleTypeDef *htim, uint32_t channel,
                uint16_t min_us, uint16_t max_us, uint16_t start_us);

/**
  * @brief Servoyu mutlak mikrosaniye konumuna surer (sinirlar uygulanir).
  */
void Servo_SetUs(servo_t *s, int32_t us);

/**
  * @brief Guncel konuma delta ekler (hiz/rate modu icin).
  */
void Servo_AddUs(servo_t *s, int32_t delta_us);

/**
  * @brief -1000..+1000 girisini min_us..max_us araligina esler (mutlak mod).
  */
void Servo_SetNorm(servo_t *s, int16_t norm);

#endif /* SERVO_H */
