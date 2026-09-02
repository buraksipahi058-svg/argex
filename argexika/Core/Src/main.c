/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "usb_device.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "crsf.h"
#include "reactor.h"
#include "servo.h"
#include "haberlesme.h"      /* STM -> Jetson telemetri (su an USART6; Adim 3'te USB-CDC) */
#include <string.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/* --- Kanal atamalari ---
 * CH1/CH2 PAYLASIMLI: SURUS modunda motor (donus/gaz), LAZER modunda taret (pan/tilt).
 * Lazer modunda motorlar kilitli oldugu icin ayni stickler taret icin kullanilir. */
#define CH_STEER            1U      /* SURUS: donus      | LAZER: PAN servo  */
#define CH_THROTTLE         2U      /* SURUS: ileri/geri | LAZER: TILT servo */
/* CH3, CH4 artik kullanilmiyor (taret CH1/CH2'ye tasindi) */
#define CH_OP_MODE          5U      /* operasyon: <esik = MANUEL, >esik = OTONOM */
#define CH_SPEED_MODE       6U      /* 3 pozisyon hiz limiti (yalniz manuel surus) */
#define CH_LASER            7U      /* >esik = LAZER modu acik (motorlar kilitlenir) */
#define CH_ESTOP            9U      /* FAILSAFE anlik buton -> kilitli E-STOP */
#define CH_FIRE             10U     /* lazer atesleme tetigi (yalniz LAZER modu) */

#define OP_AUTO_THRESH_US   1500U   /* CH5 bu ustunde -> OTONOM */
#define LASER_ON_THRESH_US  1700U   /* CH7 bu ustunde -> LAZER modu */
#define ESTOP_THRESH_US     1500U   /* CH9 bu ustunde -> buton basili */
#define FIRE_THRESH_US      1700U   /* CH10 bu esigin ustunde -> ates */

/* Otonom surus (Jetson->motor) yolu. 0 iken OTONOM moda gecilir ama motorlar
 * DURUR (yalniz mod bayragi gonderilir); Jetson surus kodu hazir olunca 1 yap. */
#define AUTO_DRIVE_ENABLED  0

/* --- Surus davranisi --- */
#define STICK_DEADBAND_US   25U     /* stick merkez olu bandi */
#define TURN_GAIN_PCT       60      /* donus keskinligi (%). 100 = tam diferansiyel */
#define DRIVE_MAX_PCT_LOW   30      /* CH6 asagi  : ilk testler icin */
#define DRIVE_MAX_PCT_MID   60      /* CH6 orta   */
#define DRIVE_MAX_PCT_HIGH  100     /* CH6 yukari */

/* Motor yon terslemeleri: tekerlekler ters donerse -1 yapin. */
#define LEFT_INV_A          (+1)
#define LEFT_INV_B          (+1)
#define RIGHT_INV_A         (-1)
#define RIGHT_INV_B         (-1)

/* --- Servo mekanik sinirlari (RDS3235: 500-2500us ~ 270 derece) --- */
#define PAN_MIN_US          700U
#define PAN_MAX_US          2300U
#define PAN_CENTER_US       1500U

#define TILT_MIN_US         1100U   /* asagi bakis siniri  */
#define TILT_MAX_US         1900U   /* yukari bakis siniri */
#define TILT_CENTER_US      1500U

/* PAN "rate" modu: stick sapmasini hiz komutu olarak yorumlar. */
#define PAN_RATE_MODE       1
#define PAN_RATE_US_PER_S   400     /* tam stick -> saniyede 400us hareket */

/* TILT tirtikli (throttle) stickte oldugu icin mutlak esleme */
#define TILT_RATE_MODE      0
#define TILT_RATE_US_PER_S  300

/* --- Zamanlama / failsafe --- */
#define LOOP_PERIOD_MS      10U     /* 100 Hz ana dongu */
#define CRSF_TIMEOUT_MS     300U    /* bu sure sinyal yoksa FAILSAFE */
#define CRSF_RX_BUF_SIZE    256U
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
TIM_HandleTypeDef htim4;

UART_HandleTypeDef huart1;
UART_HandleTypeDef huart2;
UART_HandleTypeDef huart3;
DMA_HandleTypeDef hdma_usart2_rx;

/* USER CODE BEGIN PV */
static crsf_t    g_crsf;
static reactor_t g_left;
static reactor_t g_right;
static servo_t   g_pan;
static servo_t   g_tilt;

static uint8_t   g_rx_buf[CRSF_RX_BUF_SIZE];
static uint16_t  g_rx_tail = 0;

/* Telemetri icin son UYGULANAN palet hizlari (-1000..+1000; dur = 0). */
static int16_t   g_applied_left  = 0;
static int16_t   g_applied_right = 0;

/* Mod/durum (Control_Decide her dongu gunceller). */
static uint8_t   g_laser_mode  = 0;  /* 0 = surus, 1 = lazer modu (CH7) */
static uint8_t   g_auto_active = 0;  /* 0 = manuel, 1 = otonom (CH5) -> telemetri ST_AUTO_EN */
static uint8_t   g_estop       = 0;  /* 1 = kilitli acil durdurma (CH9 buton) */
static uint8_t   g_fire_on     = 0;  /* 0/1 lazer atesleme cikisi (PE1) */
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_USART2_UART_Init(void);
static void MX_TIM4_Init(void);
static void MX_USART1_UART_Init(void);
static void MX_USART3_UART_Init(void);
/* USER CODE BEGIN PFP */
static void CRSF_PumpDMA(void);
static void Control_Decide(uint32_t now_ms, uint8_t link_ok);
static void Drive_Update(uint32_t now_ms, uint8_t link_ok);
static void Turret_Update(uint32_t now_ms, uint8_t link_ok);
static void Laser_Update(uint32_t now_ms, uint8_t link_ok);
static void Telemetry_Update(uint32_t now_ms, uint8_t link_ok);
static uint8_t Servo_UsToDeg(const servo_t *s);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_USART2_UART_Init();
  MX_TIM4_Init();
  MX_USART1_UART_Init();
  MX_USART3_UART_Init();
  MX_USB_DEVICE_Init();
  /* USER CODE BEGIN 2 */
  /* --- Modulleri baslat --- */
  CRSF_Init(&g_crsf);
  Reactor_Init(&g_left,  &huart1, LEFT_INV_A,  LEFT_INV_B);
  Reactor_Init(&g_right, &huart3, RIGHT_INV_A, RIGHT_INV_B);

  Servo_Init(&g_pan,  &htim4, TIM_CHANNEL_1, PAN_MIN_US,  PAN_MAX_US,  PAN_CENTER_US);
  Servo_Init(&g_tilt, &htim4, TIM_CHANNEL_2, TILT_MIN_US, TILT_MAX_US, TILT_CENTER_US);

  /* Guvenlik: acilista motorlar kesinlikle dursun, lazer kapali olsun */
  Reactor_Stop(&g_left);
  Reactor_Stop(&g_right);
  HAL_GPIO_WritePin(GPIOE, GPIO_PIN_0, GPIO_PIN_RESET);   /* EN = pasif   */
  HAL_GPIO_WritePin(GPIOE, GPIO_PIN_1, GPIO_PIN_RESET);   /* ATES = kapali */

  /* Jetson telemetri hattini baslat (su an USART6/PC6; Adim 3'te USB-CDC olacak) */
  Haberlesme_Init();

  /* CRSF alimini dairesel DMA ile baslat */
  if (HAL_UART_Receive_DMA(&huart2, g_rx_buf, CRSF_RX_BUF_SIZE) != HAL_OK)
  {
    Error_Handler();
  }
  g_rx_tail = 0;

  uint32_t last_loop = HAL_GetTick();
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    /* 1) Gelen CRSF byte'larini surekli isle (dongu hizindan bagimsiz) */
    CRSF_PumpDMA();

    uint32_t now = HAL_GetTick();

    /* 1b) Jetson -> STM COMMAND/HEARTBEAT: DMA tamponunu her spin bosalt
           (non-blocking). Gelen komut JetsonKomut'a yazilir; bu surumde
           henuz surus/servoya UYGULANMAZ (arbitrasyon sonraki adim). */
    Haberlesme_Poll(now);

    if ((now - last_loop) < LOOP_PERIOD_MS)
    {
      continue;
    }
    last_loop = now;

    /* 2) Baglanti durumu */
    uint8_t link_ok = CRSF_IsLinkUp(&g_crsf, now, CRSF_TIMEOUT_MS) ? 1U : 0U;

    /* 3) Once mod/E-STOP karari, sonra cikislari guncelle */
    Control_Decide(now, link_ok);
    Drive_Update(now, link_ok);
    Turret_Update(now, link_ok);
    Laser_Update(now, link_ok);

    /* 4) Durum LED'leri */
    HAL_GPIO_WritePin(GPIOD, GPIO_PIN_14, link_ok ? GPIO_PIN_RESET : GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOD, GPIO_PIN_15, link_ok ? GPIO_PIN_SET   : GPIO_PIN_RESET);

    /* 5) Jetson telemetrisi (STATUS 20 Hz + HEARTBEAT 10 Hz) */
    Telemetry_Update(now, link_ok);
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_BYPASS;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 336;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 7;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief TIM4 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM4_Init(void)
{

  /* USER CODE BEGIN TIM4_Init 0 */

  /* USER CODE END TIM4_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};
  TIM_OC_InitTypeDef sConfigOC = {0};

  /* USER CODE BEGIN TIM4_Init 1 */

  /* USER CODE END TIM4_Init 1 */
  htim4.Instance = TIM4;
  htim4.Init.Prescaler = 83;
  htim4.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim4.Init.Period = 19999;
  htim4.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim4.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
  if (HAL_TIM_Base_Init(&htim4) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim4, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_TIM_PWM_Init(&htim4) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim4, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sConfigOC.OCMode = TIM_OCMODE_PWM1;
  sConfigOC.Pulse = 1500;
  sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
  sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
  if (HAL_TIM_PWM_ConfigChannel(&htim4, &sConfigOC, TIM_CHANNEL_1) != HAL_OK)
  {
    Error_Handler();
  }
  sConfigOC.Pulse = 0;
  if (HAL_TIM_PWM_ConfigChannel(&htim4, &sConfigOC, TIM_CHANNEL_2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM4_Init 2 */

  /* USER CODE END TIM4_Init 2 */
  HAL_TIM_MspPostInit(&htim4);

}

/**
  * @brief USART1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART1_UART_Init(void)
{

  /* USER CODE BEGIN USART1_Init 0 */

  /* USER CODE END USART1_Init 0 */

  /* USER CODE BEGIN USART1_Init 1 */

  /* USER CODE END USART1_Init 1 */
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 38400;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART1_Init 2 */

  /* USER CODE END USART1_Init 2 */

}

/**
  * @brief USART2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART2_UART_Init(void)
{

  /* USER CODE BEGIN USART2_Init 0 */

  /* USER CODE END USART2_Init 0 */

  /* USER CODE BEGIN USART2_Init 1 */

  /* USER CODE END USART2_Init 1 */
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 420000;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART2_Init 2 */

  /* USER CODE END USART2_Init 2 */

}

/**
  * @brief USART3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART3_UART_Init(void)
{

  /* USER CODE BEGIN USART3_Init 0 */

  /* USER CODE END USART3_Init 0 */

  /* USER CODE BEGIN USART3_Init 1 */

  /* USER CODE END USART3_Init 1 */
  huart3.Instance = USART3;
  huart3.Init.BaudRate = 38400;
  huart3.Init.WordLength = UART_WORDLENGTH_8B;
  huart3.Init.StopBits = UART_STOPBITS_1;
  huart3.Init.Parity = UART_PARITY_NONE;
  huart3.Init.Mode = UART_MODE_TX;
  huart3.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart3.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart3) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART3_Init 2 */

  /* USER CODE END USART3_Init 2 */

}

/**
  * Enable DMA controller clock
  */
static void MX_DMA_Init(void)
{

  /* DMA controller clock enable */
  __HAL_RCC_DMA1_CLK_ENABLE();

  /* DMA interrupt init */
  /* DMA1_Stream5_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA1_Stream5_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA1_Stream5_IRQn);

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_GPIOE_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOD, GPIO_PIN_14|GPIO_PIN_15, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOE, GPIO_PIN_0, GPIO_PIN_RESET);

  /*Configure GPIO pins : PD14 PD15 */
  GPIO_InitStruct.Pin = GPIO_PIN_14|GPIO_PIN_15;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);

  /*Configure GPIO pin : PE0 */
  GPIO_InitStruct.Pin = GPIO_PIN_0;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */
  /* PE1: Lazer atesleme cikisi (HIGH = ates). .ioc'de tanimli olmadigindan
     CubeMX uretmez; burada elle output olarak kuruyoruz (regen-guvenli). */
  HAL_GPIO_WritePin(GPIOE, GPIO_PIN_1, GPIO_PIN_RESET);
  GPIO_InitStruct.Pin   = GPIO_PIN_1;
  GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull  = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);
  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* ==========================================================================
 *  CRSF: DMA halka tamponundan byte cekip parser'a besle
 * ========================================================================== */
static void CRSF_PumpDMA(void)
{
    /* DMA hata nedeniyle durduysa yeniden baslat (ORE/NE/FE korumasi) */
    if (huart2.RxState != HAL_UART_STATE_BUSY_RX)
    {
        __HAL_UART_CLEAR_OREFLAG(&huart2);
        HAL_UART_Receive_DMA(&huart2, g_rx_buf, CRSF_RX_BUF_SIZE);
        g_rx_tail = 0;
        return;
    }

    uint16_t head = (uint16_t)(CRSF_RX_BUF_SIZE - __HAL_DMA_GET_COUNTER(huart2.hdmarx));
    uint32_t now  = HAL_GetTick();

    while (g_rx_tail != head)
    {
        CRSF_ParseByte(&g_crsf, g_rx_buf[g_rx_tail], now);
        g_rx_tail++;
        if (g_rx_tail >= CRSF_RX_BUF_SIZE)
        {
            g_rx_tail = 0;
        }
    }
}

/* ==========================================================================
 *  MOD / E-STOP KARARI
 *   - CH9 anlik buton -> kilitli E-STOP (bir bas: her sey durur+kilitlenir;
 *     reset: butonu birak + CH5'i MANUEL konuma al = operator kontrolu geri aldi)
 *   - CH7 > esik  -> LAZER modu (ONCELIKLI; motorlar kilitlenir)
 *   - CH5 > esik  -> OTONOM, degilse MANUEL (yalniz lazer kapaliyken gecerli)
 *  Sonuc: g_estop / g_laser_mode / g_auto_active. Uygulama Drive/Turret/Laser
 *  _Update icinde bu bayraklara gore yapilir.
 * ========================================================================== */
static void Control_Decide(uint32_t now_ms, uint8_t link_ok)
{
    (void)now_ms;
    static uint8_t prev_btn = 0U;

    /* Link yoksa: mod bayraklari guvenli varsayilana. E-STOP mandali korunur
       (link donunce buton hala basiliysa yeniden tetiklenir). */
    if (!link_ok)
    {
        g_laser_mode  = 0U;
        g_auto_active = 0U;
        prev_btn      = 0U;
        return;
    }

    /* --- E-STOP: anlik butonun YUKSELEN kenari mandalliyor --- */
    uint8_t btn = (CRSF_GetChannelUs(&g_crsf, CH_ESTOP) > ESTOP_THRESH_US) ? 1U : 0U;
    if (btn && !prev_btn) { g_estop = 1U; }
    prev_btn = btn;

    uint8_t laser_on = (CRSF_GetChannelUs(&g_crsf, CH_LASER)   > LASER_ON_THRESH_US) ? 1U : 0U;
    uint8_t auto_on  = (CRSF_GetChannelUs(&g_crsf, CH_OP_MODE) > OP_AUTO_THRESH_US)  ? 1U : 0U;

    /* Reset: buton birakildi VE CH5 MANUEL konumda */
    if (g_estop && !btn && !auto_on) { g_estop = 0U; }

    /* Oncelik: LAZER (CH7) > operasyon (CH5). Lazer acikken otonom devre disi. */
    g_laser_mode  = laser_on;
    g_auto_active = (!laser_on && auto_on) ? 1U : 0U;
}

/* ==========================================================================
 *  SURUS: tank (skid-steer) mix -> iki Reactor surucusu
 *   Motorlar YALNIZ manuel/otonom surus + gecerli kontrol varken doner.
 *   FAILSAFE(link) | E-STOP | LAZER modu -> her durumda DUR.
 * ========================================================================== */
static void Drive_Update(uint32_t now_ms, uint8_t link_ok)
{
    /* --- Motor DUR sartlari: sinyal yok, acil durdurma, veya lazer modu --- */
    if (!link_ok || g_estop || g_laser_mode)
    {
        Reactor_Stop(&g_left);
        Reactor_Stop(&g_right);
        HAL_GPIO_WritePin(GPIOE, GPIO_PIN_0, GPIO_PIN_RESET);   /* EN pasif */
        g_applied_left  = 0;
        g_applied_right = 0;
        return;
    }

    /* --- OTONOM modu --- */
    if (g_auto_active)
    {
#if AUTO_DRIVE_ENABLED
        /* Jetson hedeflerini uygula (yalniz gecerli el sikismasinda) */
        const JetsonKomut *k = Haberlesme_GetKomut();
        uint8_t auto_ok = (k->bayrak & CMD_FLAG_AUTO_REQ)
                          && Haberlesme_KomutTaze(now_ms)
                          && Haberlesme_JetsonLinkTaze(now_ms);
        if (auto_ok)
        {
            int32_t l = (int32_t)k->solHedef * 10;   /* -100..100 -> -1000..1000 */
            int32_t r = (int32_t)k->sagHedef * 10;
            if (l >  1000) { l =  1000; } if (l < -1000) { l = -1000; }
            if (r >  1000) { r =  1000; } if (r < -1000) { r = -1000; }
            Reactor_SetSpeed(&g_left,  (int16_t)l, (int16_t)l);
            Reactor_SetSpeed(&g_right, (int16_t)r, (int16_t)r);
            HAL_GPIO_WritePin(GPIOE, GPIO_PIN_0, GPIO_PIN_SET);
            g_applied_left  = (int16_t)l;
            g_applied_right = (int16_t)r;
            return;
        }
#else
        (void)now_ms;
#endif
        /* Jetson surusu hazir/gecerli degil -> guvenli DUR (mod yine OTONOM) */
        Reactor_Stop(&g_left);
        Reactor_Stop(&g_right);
        HAL_GPIO_WritePin(GPIOE, GPIO_PIN_0, GPIO_PIN_RESET);
        g_applied_left  = 0;
        g_applied_right = 0;
        return;
    }

    /* --- MANUEL surus: RC stickleri (CH1 donus, CH2 gaz) --- */
    HAL_GPIO_WritePin(GPIOE, GPIO_PIN_0, GPIO_PIN_SET);   /* EN aktif */

    /* Hiz limiti (CH6, 3 pozisyonlu anahtar) */
    uint16_t sp_us   = CRSF_GetChannelUs(&g_crsf, CH_SPEED_MODE);
    int32_t  max_pct = DRIVE_MAX_PCT_LOW;
    if      (sp_us > 1700U) { max_pct = DRIVE_MAX_PCT_HIGH; }
    else if (sp_us > 1300U) { max_pct = DRIVE_MAX_PCT_MID;  }

    int32_t thr = CRSF_GetChannelNorm(&g_crsf, CH_THROTTLE, STICK_DEADBAND_US);
    int32_t str = CRSF_GetChannelNorm(&g_crsf, CH_STEER,    STICK_DEADBAND_US);

    str = (str * TURN_GAIN_PCT) / 100;

    /* Tank mix */
    int32_t left  = thr + str;
    int32_t right = thr - str;

    /* Tasma olursa iki tarafi birlikte olcekle (donus hissi bozulmasin) */
    int32_t peak = (left  >  0) ?  left  : -left;
    int32_t tmp  = (right >  0) ?  right : -right;
    if (tmp > peak) { peak = tmp; }
    if (peak > 1000)
    {
        left  = (left  * 1000) / peak;
        right = (right * 1000) / peak;
    }

    left  = (left  * max_pct) / 100;
    right = (right * max_pct) / 100;

    Reactor_SetSpeed(&g_left,  (int16_t)left,  (int16_t)left);
    Reactor_SetSpeed(&g_right, (int16_t)right, (int16_t)right);

    g_applied_left  = (int16_t)left;
    g_applied_right = (int16_t)right;
}

/* ==========================================================================
 *  TARET: pan (yatay) / tilt (dikey) servolari
 *   Taret YALNIZ lazer modunda kumanda edilir; stickler CH1 (pan) / CH2 (tilt).
 *   Diger modlarda (surus) servolar SON KONUMDA kalir. Lazerde motorlar zaten
 *   kilitli oldugu icin ayni CH1/CH2 stickleri taret icin kullanilir.
 * ========================================================================== */
static void Turret_Update(uint32_t now_ms, uint8_t link_ok)
{
    (void)now_ms;

    /* Lazer modu disinda, link yokken veya E-STOP'ta servolar son konumda kalir */
    if (!link_ok || g_estop || !g_laser_mode)
    {
        return;
    }

    int32_t pan_in  = CRSF_GetChannelNorm(&g_crsf, CH_STEER,    STICK_DEADBAND_US); /* CH1 */
    int32_t tilt_in = CRSF_GetChannelNorm(&g_crsf, CH_THROTTLE, STICK_DEADBAND_US); /* CH2 */

    /* --- PAN --- */
#if PAN_RATE_MODE
    /* delta = rate * stick * dt   (dt = LOOP_PERIOD_MS) */
    int32_t d_pan = (PAN_RATE_US_PER_S * pan_in * (int32_t)LOOP_PERIOD_MS) / (1000 * 1000);
    Servo_AddUs(&g_pan, d_pan);
#else
    Servo_SetNorm(&g_pan, (int16_t)pan_in);
#endif

    /* --- TILT --- */
#if TILT_RATE_MODE
    int32_t d_tilt = (TILT_RATE_US_PER_S * tilt_in * (int32_t)LOOP_PERIOD_MS) / (1000 * 1000);
    Servo_AddUs(&g_tilt, d_tilt);
#else
    Servo_SetNorm(&g_tilt, (int16_t)tilt_in);
#endif
}

/* ==========================================================================
 *  LAZER ATES: yalniz LAZER modunda (CH7) CH10 tetigi ile PE1 -> HIGH.
 *   Mod karari Control_Decide'da verilir; burada sadece atesleme uygulanir.
 *   Lazer modu disinda / E-STOP / link yok -> ates kapali.
 * ========================================================================== */
static void Laser_Update(uint32_t now_ms, uint8_t link_ok)
{
    (void)now_ms;

    if (!link_ok || g_estop || !g_laser_mode)
    {
        HAL_GPIO_WritePin(GPIOE, GPIO_PIN_1, GPIO_PIN_RESET);
        g_fire_on = 0U;
        return;
    }

    /* LAZER modu: CH10 tetigi ile atesle */
    uint8_t fire = (CRSF_GetChannelUs(&g_crsf, CH_FIRE) > FIRE_THRESH_US) ? 1U : 0U;
    g_fire_on    = fire;
    HAL_GPIO_WritePin(GPIOE, GPIO_PIN_1, fire ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

/* ==========================================================================
 *  TELEMETRI: Jetson'a STATUS (20 Hz) + HEARTBEAT (10 Hz)
 * ========================================================================== */

/* Servo darbe genisligini (min_us..max_us) 0..180 dereceye esler. */
static uint8_t Servo_UsToDeg(const servo_t *s)
{
    int32_t span = (int32_t)s->max_us - (int32_t)s->min_us;
    if (span <= 0) { return 90U; }
    int32_t deg = ((int32_t)s->pos_us - (int32_t)s->min_us) * 180 / span;
    if (deg < 0)   { deg = 0;   }
    if (deg > 180) { deg = 180; }
    return (uint8_t)deg;
}

static void Telemetry_Update(uint32_t now_ms, uint8_t link_ok)
{
    static uint32_t t_status = 0;
    static uint32_t t_hb     = 0;

    if ((now_ms - t_status) >= STATUS_PERIOD_MS)   /* 20 Hz */
    {
        t_status = now_ms;

        VehicleState st;
        st.solMotor = (int8_t)(g_applied_left  / 10);   /* -1000..1000 -> -100..100 */
        st.sagMotor = (int8_t)(g_applied_right / 10);
        st.pan      = Servo_UsToDeg(&g_pan);
        st.tilt     = Servo_UsToDeg(&g_tilt);
        st.lazer    = g_fire_on;                        /* CH10 atesleme durumu (PE1) */
        st.aktifMod = g_laser_mode;                     /* 0 = surus, 1 = lazer modu */
        st.elrsLink = link_ok ? 1U : 0U;

        uint8_t durum = link_ok ? 0U : ST_FAILSAFE;
        if (g_estop)       { durum |= ST_FAILSAFE; }   /* E-STOP da FAILSAFE gorunur */
        if (g_auto_active) { durum |= ST_AUTO_EN;  }   /* otonom aktif */
        if (Haberlesme_JetsonLinkTaze(now_ms)) { durum |= ST_JETSON_LINK; }
        st.durum    = durum;
        Haberlesme_SendStatus(&st);
    }

    if ((now_ms - t_hb) >= HB_PERIOD_MS)               /* 10 Hz */
    {
        t_hb = now_ms;
        Haberlesme_SendHeartbeat(now_ms);
    }
}

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  HAL_GPIO_WritePin(GPIOD, GPIO_PIN_14, GPIO_PIN_SET);   /* PD14 kirmizi LED yanik kalir */
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
