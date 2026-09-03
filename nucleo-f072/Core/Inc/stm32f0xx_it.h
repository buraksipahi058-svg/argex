/**
  ******************************************************************************
  * @file    stm32f0xx_it.h
  * @brief   Interrupt Service Routines prototipleri (Nucleo-F072RB portu)
  *
  *  Bu dosyayi da CubeMX'in urettigi ile DEGISTIRIN (bkz. stm32f0xx_it.c basligi).
  ******************************************************************************
  */

#ifndef __STM32F0xx_IT_H
#define __STM32F0xx_IT_H

#ifdef __cplusplus
extern "C" {
#endif

void NMI_Handler(void);
void HardFault_Handler(void);
void SVC_Handler(void);
void PendSV_Handler(void);
void SysTick_Handler(void);

void USB_IRQHandler(void);
void DMA1_Channel2_3_IRQHandler(void);

#ifdef __cplusplus
}
#endif

#endif /* __STM32F0xx_IT_H */
