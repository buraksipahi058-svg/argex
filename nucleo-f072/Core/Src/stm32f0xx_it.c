/**
  ******************************************************************************
  * @file    stm32f0xx_it.c
  * @brief   Interrupt Service Routines (Nucleo-F072RB portu)
  *
  *  Cortex-M0 cekirdeginde F4'teki MemManage/BusFault/UsageFault/DebugMon
  *  handler'lari YOKTUR; yalniz NMI/HardFault/SVC/PendSV/SysTick vardir.
  *
  *  DMA yonlendirmesi:
  *    DMA1 Channel3 (USART1_RX / CRSF)  -> DMA1_Channel2_3_IRQHandler
  *    DMA1 Channel5 (USART2_RX / Jetson)-> DMA1_Channel4_5_6_7_IRQHandler
  ******************************************************************************
  */

#include "main.h"
#include "stm32f0xx_it.h"
#include "haberlesme.h"        /* Haberlesme_DmaRxIrq() (USART2 RX DMA kancasi) */

/* CRSF (USART1_RX) DMA handle - main.c'de tanimli. */
extern DMA_HandleTypeDef hdma_crsf_rx;

/******************************************************************************/
/*           Cortex-M0 Processor Interruption and Exception Handlers         */
/******************************************************************************/

void NMI_Handler(void)
{
    while (1)
    {
    }
}

void HardFault_Handler(void)
{
    while (1)
    {
    }
}

void SVC_Handler(void)
{
}

void PendSV_Handler(void)
{
}

void SysTick_Handler(void)
{
    HAL_IncTick();
}

/******************************************************************************/
/* STM32F0xx Peripheral Interrupt Handlers                                    */
/******************************************************************************/

/**
  * @brief DMA1 Channel2/3 ortak kesmesi - CRSF (USART1_RX, Channel3).
  */
void DMA1_Channel2_3_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&hdma_crsf_rx);
}

/**
  * @brief DMA1 Channel4/5/6/7 ortak kesmesi - Jetson (USART2_RX, Channel5).
  */
void DMA1_Channel4_5_6_7_IRQHandler(void)
{
    Haberlesme_DmaRxIrq();     /* -> HAL_DMA_IRQHandler(&s_hdma_rx) */
}
