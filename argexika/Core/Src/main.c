/**
  ******************************************************************************
  * @file    main.c
  * @brief   TEKNOFEST IKA - STM32F407 Disc-1
  *          RadioMaster TX12 MKII (ELRS/CRSF) -> 2x Reactor motor surucu (UART)
  *                                            -> 2x RDS3235 servo (pan/tilt)
  *
  *  PIN HARITASI
  *  ------------------------------------------------------------------
  *  PA3   USART2_RX   420000 baud  <- ELRS alici TX pedi (CRSF)   [DMA circ.]
  *  PB6   USART1_TX    38400 baud  -> SOL Reactor  "SRL" pini
  *  PD8   USART3_TX    38400 baud  -> SAG Reactor  "SRL" pini
  *  PD12  TIM4_CH1     50 Hz PWM   -> PAN  (yatay) RDS3235 sinyal
  *  PD13  TIM4_CH2     50 Hz PWM   -> TILT (dikey) RDS3235 sinyal
  *  PE0   GPIO out                 -> (ops.) Reactor EN pini / role surucu
  *  PD14  GPIO out                 -> Kirmizi LED : FAILSAFE
  *  PD15  GPIO out                 -> Mavi LED    : LINK OK / ARM
  *
  *  KUMANDA KANALLARI (EdgeTX, Mode 2, AETR)
  *  ------------------------------------------------------------------
  *  CH1  Aileron  (sag stick yatay)  -> DONUS  (tank mix)
  *  CH2  Elevator (sag stick dikey)  -> ILERI / GERI
  *  CH3  Throttle (sol stick dikey)  -> TILT  (dikey eksen)  [tirtikli, konum tutar]
  *  CH4  Rudder   (sol stick yatay)  -> PAN   (yatay eksen)  [merkeze doner]
  *  CH5  AUX1 (SA)                   -> ARM  (>1700us = motorlar aktif)
  *  CH6  AUX2 (SB, 3 pozisyon)       -> HIZ LIMITI  (dusuk/orta/yuksek)
  *  CH7  AUX3 (SC)                   -> Servo merkeze donus (>1700us)
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "crsf.h"
#include "reactor.h"
#include "servo.h"
#include "haberlesme.h"      /* STM -> Jetson telemetri (USART6 / PC6) */
#include <string.h>

/* ==========================================================================
 *  AYARLANABILIR PARAMETRELER  -  ilk denemede burayi gozden gecirin
 * ========================================================================== */

/* --- Kanal atamalari --- */
#define CH_STEER            1U      /* donus */
#define CH_THROTTLE         2U      /* ileri/geri */
#define CH_TILT             3U      /* dikey eksen servo */
#define CH_PAN              4U      /* yatay eksen servo */
#define CH_ARM              5U      /* silahlandirma anahtari */
#define CH_SPEED_MODE       6U      /* 3 pozisyon hiz limiti */
#define CH_TURRET_CENTER    7U      /* servolari merkeze al */

/* --- Surus davranisi --- */
#define STICK_DEADBAND_US   25U     /* stick merkez olu bandi */
#define TURN_GAIN_PCT       60      /* donus keskinligi (%). 100 = tam diferansiyel */
#define DRIVE_MAX_PCT_LOW   30      /* CH6 asagi  : ilk testler icin */
#define DRIVE_MAX_PCT_MID   60      /* CH6 orta   */
#define DRIVE_MAX_PCT_HIGH  100     /* CH6 yukari */

/* Motor yon terslemeleri: tekerlekler ters donerse -1 yapin.
   Sol ve sag sasi motorlari birbirinin aynasi oldugu icin genelde
   bir taraf +1, diger taraf -1 olur. Once havada test edin! */
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

/* PAN "rate" modu: stick merkeze donen bir eksen oldugu icin
   stick sapmasini hiz komutu olarak yorumlar (nisan almak icin daha rahat).
   0 yaparsaniz stick konumu dogrudan aci olarak eslenir. */
#define PAN_RATE_MODE       1
#define PAN_RATE_US_PER_S   400     /* tam stick -> saniyede 400us hareket */

/* TILT tirtikli (throttle) stickte oldugu icin mutlak esleme daha uygun */
#define TILT_RATE_MODE      0
#define TILT_RATE_US_PER_S  300

/* --- Zamanlama / failsafe --- */
#define LOOP_PERIOD_MS      10U     /* 100 Hz ana dongu */
#define CRSF_TIMEOUT_MS     300U    /* bu sure sinyal yoksa FAILSAFE */
#define CRSF_RX_BUF_SIZE    256U

/* ==========================================================================
 *  Global degiskenler
 * ========================================================================== */
UART_HandleTypeDef huart1;   /* SOL Reactor  */
UART_HandleTypeDef huart2;   /* CRSF alici   */
UART_HandleTypeDef huart3;   /* SAG Reactor  */
/* NOT: CubeMX surumunuze gore bu satir stm32f4xx_hal_msp.c icinde de
   uretilmis olabilir. Derlerken "multiple definition of hdma_usart2_rx"
   hatasi alirsaniz asagidaki satiri silip yerine su satiri yazin:
       extern DMA_HandleTypeDef hdma_usart2_rx;                            */
DMA_HandleTypeDef  hdma_usart2_rx;
TIM_HandleTypeDef  htim4;    /* servo PWM    */

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

/* ==========================================================================
 *  Fonksiyon prototipleri
 * ========================================================================== */
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_USART1_UART_Init(void);
static void MX_USART2_UART_Init(void);
static void MX_USART3_UART_Init(void);
static void MX_TIM4_Init(void);

/* Bu ikisi CubeMX'in urettigi diger dosyalardan da cagrildigi icin
   'static' OLMAMALIDIR (main.h icinde de bildirilirler). */
void Error_Handler(void);
void HAL_TIM_MspPostInit(TIM_HandleTypeDef *htim);   /* stm32f4xx_hal_msp.c icinde uretilir */

static void CRSF_PumpDMA(void);
static void Drive_Update(uint32_t now_ms, uint8_t link_ok);
static void Turret_Update(uint32_t now_ms, uint8_t link_ok);
static void Telemetry_Update(uint32_t now_ms, uint8_t link_ok);
static uint8_t Servo_UsToDeg(const servo_t *s);

/* ==========================================================================
 *  MAIN
 * ========================================================================== */
int main(void)
{
    HAL_Init();
    SystemClock_Config();

    MX_GPIO_Init();
    MX_DMA_Init();
    MX_USART1_UART_Init();
    MX_USART2_UART_Init();
    MX_USART3_UART_Init();
    MX_TIM4_Init();

    /* --- Modulleri baslat --- */
    CRSF_Init(&g_crsf);
    Reactor_Init(&g_left,  &huart1, LEFT_INV_A,  LEFT_INV_B);
    Reactor_Init(&g_right, &huart3, RIGHT_INV_A, RIGHT_INV_B);

    Servo_Init(&g_pan,  &htim4, TIM_CHANNEL_1, PAN_MIN_US,  PAN_MAX_US,  PAN_CENTER_US);
    Servo_Init(&g_tilt, &htim4, TIM_CHANNEL_2, TILT_MIN_US, TILT_MAX_US, TILT_CENTER_US);

    /* Guvenlik: acilista motorlar kesinlikle dursun */
    Reactor_Stop(&g_left);
    Reactor_Stop(&g_right);
    HAL_GPIO_WritePin(GPIOE, GPIO_PIN_0, GPIO_PIN_RESET);   /* EN = pasif */

    /* Jetson telemetri hattini baslat (USART6 / PC6 -> Jetson RX, tek yonlu) */
    Haberlesme_Init();

    /* CRSF alimini dairesel DMA ile baslat */
    if (HAL_UART_Receive_DMA(&huart2, g_rx_buf, CRSF_RX_BUF_SIZE) != HAL_OK)
    {
        Error_Handler();
    }
    g_rx_tail = 0;

    uint32_t last_loop = HAL_GetTick();

    while (1)
    {
        /* 1) Gelen CRSF byte'larini surekli isle (dongu hizindan bagimsiz) */
        CRSF_PumpDMA();

        uint32_t now = HAL_GetTick();
        if ((now - last_loop) < LOOP_PERIOD_MS)
        {
            continue;
        }
        last_loop = now;

        /* 2) Baglanti durumu */
        uint8_t link_ok = CRSF_IsLinkUp(&g_crsf, now, CRSF_TIMEOUT_MS) ? 1U : 0U;

        /* 3) Cikislari guncelle */
        Drive_Update(now, link_ok);
        Turret_Update(now, link_ok);

        /* 4) Durum LED'leri */
        HAL_GPIO_WritePin(GPIOD, GPIO_PIN_14, link_ok ? GPIO_PIN_RESET : GPIO_PIN_SET);
        HAL_GPIO_WritePin(GPIOD, GPIO_PIN_15, link_ok ? GPIO_PIN_SET   : GPIO_PIN_RESET);

        /* 5) Jetson telemetrisi (STATUS 20 Hz + HEARTBEAT 10 Hz) */
        Telemetry_Update(now, link_ok);
    }
}

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
 *  SURUS: tank (skid-steer) mix -> iki Reactor surucusu
 * ========================================================================== */
static void Drive_Update(uint32_t now_ms, uint8_t link_ok)
{
    (void)now_ms;

    /* --- FAILSAFE: sinyal yok -> her sey dursun --- */
    if (!link_ok)
    {
        Reactor_Stop(&g_left);
        Reactor_Stop(&g_right);
        HAL_GPIO_WritePin(GPIOE, GPIO_PIN_0, GPIO_PIN_RESET);
        g_applied_left  = 0;   /* telemetri: motorlar durdu */
        g_applied_right = 0;
        return;
    }

    /* --- ARM anahtari --- */
    uint8_t armed = (CRSF_GetChannelUs(&g_crsf, CH_ARM) > 1700U) ? 1U : 0U;
    HAL_GPIO_WritePin(GPIOE, GPIO_PIN_0, armed ? GPIO_PIN_SET : GPIO_PIN_RESET);

    if (!armed)
    {
        Reactor_Stop(&g_left);
        Reactor_Stop(&g_right);
        g_applied_left  = 0;   /* telemetri: silahsiz -> motorlar durdu */
        g_applied_right = 0;
        return;
    }

    /* --- Hiz limiti (CH6, 3 pozisyonlu anahtar) --- */
    uint16_t sp_us   = CRSF_GetChannelUs(&g_crsf, CH_SPEED_MODE);
    int32_t  max_pct = DRIVE_MAX_PCT_LOW;
    if      (sp_us > 1700U) { max_pct = DRIVE_MAX_PCT_HIGH; }
    else if (sp_us > 1300U) { max_pct = DRIVE_MAX_PCT_MID;  }

    /* --- Stickler --- */
    int32_t thr = CRSF_GetChannelNorm(&g_crsf, CH_THROTTLE, STICK_DEADBAND_US);
    int32_t str = CRSF_GetChannelNorm(&g_crsf, CH_STEER,    STICK_DEADBAND_US);

    str = (str * TURN_GAIN_PCT) / 100;

    /* --- Tank mix --- */
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

    /* --- Hiz limitini uygula --- */
    left  = (left  * max_pct) / 100;
    right = (right * max_pct) / 100;

    /* Sol surucunun HER IKI kanali sol paletin motorlarini,
       sag surucununkiler sag paletin motorlarini surer. */
    Reactor_SetSpeed(&g_left,  (int16_t)left,  (int16_t)left);
    Reactor_SetSpeed(&g_right, (int16_t)right, (int16_t)right);

    g_applied_left  = (int16_t)left;    /* telemetri: uygulanan hizlar */
    g_applied_right = (int16_t)right;
}

/* ==========================================================================
 *  TARET: pan (yatay) / tilt (dikey) servolari
 * ========================================================================== */
static void Turret_Update(uint32_t now_ms, uint8_t link_ok)
{
    (void)now_ms;

    /* Sinyal yoksa servolar SON KONUMDA kalir (taret sarsilmasin) */
    if (!link_ok)
    {
        return;
    }

    /* Merkeze donus anahtari */
    if (CRSF_GetChannelUs(&g_crsf, CH_TURRET_CENTER) > 1700U)
    {
        Servo_SetUs(&g_pan,  PAN_CENTER_US);
        Servo_SetUs(&g_tilt, TILT_CENTER_US);
        return;
    }

    int32_t pan_in  = CRSF_GetChannelNorm(&g_crsf, CH_PAN,  STICK_DEADBAND_US);
    int32_t tilt_in = CRSF_GetChannelNorm(&g_crsf, CH_TILT, STICK_DEADBAND_US);

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
 *  TELEMETRI: Jetson'a STATUS (20 Hz) + HEARTBEAT (10 Hz)
 *  Tek yonlu: STM sadece gonderir. Durum bitlerinden yalnizca FAILSAFE
 *  uretilir (Jetson komutu/otonomi yok -> digerleri 0). Eski Arduino
 *  firmware'inin aracDurumGuncelle() mantiginin native karsiligidir.
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
        st.lazer    = 0U;                               /* bu firmware'de lazer/ates cikisi yok */
        st.aktifMod = 0U;                               /* yalnizca surus modu */
        st.elrsLink = link_ok ? 1U : 0U;
        st.durum    = link_ok ? 0U : ST_FAILSAFE;       /* diger bitler bu firmware'de 0 */
        Haberlesme_SendStatus(&st);
    }

    if ((now_ms - t_hb) >= HB_PERIOD_MS)               /* 10 Hz */
    {
        t_hb = now_ms;
        Haberlesme_SendHeartbeat(now_ms);
    }
}

/* ==========================================================================
 *  HAL / CubeMX init fonksiyonlari
 * ========================================================================== */

void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

    /* Disc-1 karti uzerinde 8 MHz HSE (ST-LINK MCO) */
    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    RCC_OscInitStruct.HSEState       = RCC_HSE_BYPASS;
    RCC_OscInitStruct.PLL.PLLState   = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource  = RCC_PLLSOURCE_HSE;
    RCC_OscInitStruct.PLL.PLLM       = 8;
    RCC_OscInitStruct.PLL.PLLN       = 336;
    RCC_OscInitStruct.PLL.PLLP       = RCC_PLLP_DIV2;   /* SYSCLK = 168 MHz */
    RCC_OscInitStruct.PLL.PLLQ       = 7;
    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
    {
        Error_Handler();
    }

    RCC_ClkInitStruct.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                                       RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;   /* 42 MHz  (TIM: 84 MHz) */
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;   /* 84 MHz */
    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
    {
        Error_Handler();
    }
}

/* --- USART1 : SOL Reactor, 38400 --- */
static void MX_USART1_UART_Init(void)
{
    huart1.Instance          = USART1;
    huart1.Init.BaudRate     = REACTOR_BAUDRATE;
    huart1.Init.WordLength   = UART_WORDLENGTH_8B;
    huart1.Init.StopBits     = UART_STOPBITS_1;
    huart1.Init.Parity       = UART_PARITY_NONE;
    huart1.Init.Mode         = UART_MODE_TX;
    huart1.Init.HwFlowCtl    = UART_HWCONTROL_NONE;
    huart1.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(&huart1) != HAL_OK)
    {
        Error_Handler();
    }
}

/* --- USART2 : CRSF alici, 420000 --- */
static void MX_USART2_UART_Init(void)
{
    huart2.Instance          = USART2;
    huart2.Init.BaudRate     = CRSF_BAUDRATE;
    huart2.Init.WordLength   = UART_WORDLENGTH_8B;
    huart2.Init.StopBits     = UART_STOPBITS_1;
    huart2.Init.Parity       = UART_PARITY_NONE;
    huart2.Init.Mode         = UART_MODE_RX;
    huart2.Init.HwFlowCtl    = UART_HWCONTROL_NONE;
    huart2.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(&huart2) != HAL_OK)
    {
        Error_Handler();
    }
}

/* --- USART3 : SAG Reactor, 38400 --- */
static void MX_USART3_UART_Init(void)
{
    huart3.Instance          = USART3;
    huart3.Init.BaudRate     = REACTOR_BAUDRATE;
    huart3.Init.WordLength   = UART_WORDLENGTH_8B;
    huart3.Init.StopBits     = UART_STOPBITS_1;
    huart3.Init.Parity       = UART_PARITY_NONE;
    huart3.Init.Mode         = UART_MODE_TX;
    huart3.Init.HwFlowCtl    = UART_HWCONTROL_NONE;
    huart3.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(&huart3) != HAL_OK)
    {
        Error_Handler();
    }
}

/* --- TIM4 : 50 Hz servo PWM, 1 us cozunurluk --- */
static void MX_TIM4_Init(void)
{
    TIM_ClockConfigTypeDef  sClockSourceConfig = {0};
    TIM_MasterConfigTypeDef sMasterConfig      = {0};
    TIM_OC_InitTypeDef      sConfigOC          = {0};

    htim4.Instance               = TIM4;
    htim4.Init.Prescaler         = 84 - 1;      /* 84 MHz / 84 = 1 MHz  */
    htim4.Init.CounterMode       = TIM_COUNTERMODE_UP;
    htim4.Init.Period            = 20000 - 1;   /* 1 MHz / 20000 = 50 Hz */
    htim4.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
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
    sMasterConfig.MasterSlaveMode     = TIM_MASTERSLAVEMODE_DISABLE;
    if (HAL_TIMEx_MasterConfigSynchronization(&htim4, &sMasterConfig) != HAL_OK)
    {
        Error_Handler();
    }

    sConfigOC.OCMode     = TIM_OCMODE_PWM1;
    sConfigOC.Pulse      = SERVO_MID_US;
    sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
    sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;

    if (HAL_TIM_PWM_ConfigChannel(&htim4, &sConfigOC, TIM_CHANNEL_1) != HAL_OK)
    {
        Error_Handler();
    }
    if (HAL_TIM_PWM_ConfigChannel(&htim4, &sConfigOC, TIM_CHANNEL_2) != HAL_OK)
    {
        Error_Handler();
    }

    HAL_TIM_MspPostInit(&htim4);
}

/* --- DMA --- */
static void MX_DMA_Init(void)
{
    __HAL_RCC_DMA1_CLK_ENABLE();

    HAL_NVIC_SetPriority(DMA1_Stream5_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(DMA1_Stream5_IRQn);
}

/* --- GPIO --- */
static void MX_GPIO_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();

    /* Durum LED'leri: PD14 (kirmizi), PD15 (mavi) */
    HAL_GPIO_WritePin(GPIOD, GPIO_PIN_14 | GPIO_PIN_15, GPIO_PIN_RESET);
    GPIO_InitStruct.Pin   = GPIO_PIN_14 | GPIO_PIN_15;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);

    /* Reactor EN cikisi: PE0 */
    HAL_GPIO_WritePin(GPIOE, GPIO_PIN_0, GPIO_PIN_RESET);
    GPIO_InitStruct.Pin   = GPIO_PIN_0;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);
}

/* --- Hata yakalayici: motorlari durdurup bekle --- */
void Error_Handler(void)
{
    __disable_irq();
    while (1)
    {
        /* PD14 kirmizi LED yanik kalir */
        HAL_GPIO_WritePin(GPIOD, GPIO_PIN_14, GPIO_PIN_SET);
    }
}

