/**
  ******************************************************************************
  * @file    servo.c
  * @brief   RDS3235 servo PWM surucu implementasyonu
  ******************************************************************************
  */

#include "servo.h"

/* ------------------------------------------------------------------------ */
static void servo_apply(servo_t *s)
{
    __HAL_TIM_SET_COMPARE(s->htim, s->channel, s->pos_us);
}

/* ------------------------------------------------------------------------ */
void Servo_Init(servo_t *s, TIM_HandleTypeDef *htim, uint32_t channel,
                uint16_t min_us, uint16_t max_us, uint16_t start_us)
{
    if (min_us < SERVO_ABS_MIN_US) { min_us = SERVO_ABS_MIN_US; }
    if (max_us > SERVO_ABS_MAX_US) { max_us = SERVO_ABS_MAX_US; }

    s->htim    = htim;
    s->channel = channel;
    s->min_us  = min_us;
    s->max_us  = max_us;
    s->pos_us  = start_us;

    if (s->pos_us < s->min_us) { s->pos_us = s->min_us; }
    if (s->pos_us > s->max_us) { s->pos_us = s->max_us; }

    HAL_TIM_PWM_Start(s->htim, s->channel);
    servo_apply(s);
}

/* ------------------------------------------------------------------------ */
void Servo_SetUs(servo_t *s, int32_t us)
{
    if (us < (int32_t)s->min_us) { us = (int32_t)s->min_us; }
    if (us > (int32_t)s->max_us) { us = (int32_t)s->max_us; }

    s->pos_us = (uint16_t)us;
    servo_apply(s);
}

/* ------------------------------------------------------------------------ */
void Servo_AddUs(servo_t *s, int32_t delta_us)
{
    Servo_SetUs(s, (int32_t)s->pos_us + delta_us);
}

/* ------------------------------------------------------------------------ */
void Servo_SetNorm(servo_t *s, int16_t norm)
{
    if (norm >  1000) { norm =  1000; }
    if (norm < -1000) { norm = -1000; }

    int32_t mid  = ((int32_t)s->min_us + (int32_t)s->max_us) / 2;
    int32_t half = ((int32_t)s->max_us - (int32_t)s->min_us) / 2;

    Servo_SetUs(s, mid + (half * (int32_t)norm) / 1000);
}
