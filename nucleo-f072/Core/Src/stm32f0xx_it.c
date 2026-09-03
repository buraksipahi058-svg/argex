/**
  ******************************************************************************
  * @file    stm32f0xx_it.c
  * @brief   Interrupt Service Routines (Nucleo-F072RB portu)
  *
  *  Cortex-M0 cekirdegi: yalniz NMI/HardFault/SVC/PendSV/SysTick cekirdek
  *  istisnalari + kullanilan cevre birim vektorleri.
  *
  *  DIKKAT: CubeMX bu dosyayi da uretir. Bu surumle TAMAMEN DEGISTIRIN
  *  (uzerine ekleme YAPMAYIN) - aksi halde USB_IRQHandler / SysTick_Handler
  *  cift tanimlanip linker hatasi verir. Ayrinti: KURULUM.md
  ******************************************************************************
  */

#include "main.h"
#include "stm32f0xx_it.h"

/* Disaridan erisilen handle'lar */
extern PCD_HandleTypeDef hpcd_USB_FS;    /* usbd_conf.c (CubeMX) tanimlar */
extern DMA_HandleTypeDef hdma_crsf_rx;   /* main.c tanimlar (USART1_RX / DMA1 Ch3) */

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
  * @brief  USB Full-Speed global kesmesi (native USB-CDC / Jetson linki).
  */
void USB_IRQHandler(void)
{
  HAL_PCD_IRQHandler(&hpcd_USB_FS);
}

/**
  * @brief  DMA1 Channel2/3 ortak kesmesi. CRSF (USART1_RX) DMA Channel3'te.
  */
void DMA1_Channel2_3_IRQHandler(void)
{
  HAL_DMA_IRQHandler(&hdma_crsf_rx);
}
