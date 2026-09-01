/**
  ******************************************************************************
  * @file    main.c
  * @brief   TEKNOFEST IKA - Nucleo-F072RB portu (argexika/F407 firmware'inden)
  *          RadioMaster TX12 MKII (ELRS/CRSF) -> 2x Reactor motor surucu (UART)
  *                                            -> 2x RDS3235 servo (pan/tilt)
  *
  *  PIN HARITASI (F072RB) - ayrinti: nucleo-f072/../pinmodev2.md
  *  ------------------------------------------------------------------
  *  PA3   USART2_RX  115200  <-> Jetson (ST-Link VCP)  [DMA1 Ch5]  (haberlesme.c)
  *  PA2   USART2_TX  115200  <-> Jetson (ST-Link VCP)              (haberlesme.c)
  *  PA10  USART1_RX  420000  <-  ELRS alici TX pedi (CRSF)   [DMA1 Ch3]
  *  PB10  USART3_TX   38400  ->  SOL Reactor "SRL" pini
  *  PC10  USART4_TX   38400  ->  SAG Reactor "SRL" pini
  *  PB0   TIM3_CH3    50 Hz  ->  PAN  (yatay) RDS3235 sinyal
  *  PB1   TIM3_CH4    50 Hz  ->  TILT (dikey) RDS3235 sinyal
  *  PC0   GPIO out           ->  Reactor EN pini / role surucu
  *  PC3   GPIO out           ->  LAZER atesleme cikisi (HIGH = ates)
  *  PC1   GPIO out           ->  Kirmizi LED : FAILSAFE
  *  PC2   GPIO out           ->  Mavi LED    : LINK OK / ARM
  *
  *  NOT: USART2 (Jetson) donanimi haberlesme.c icinde kendi kendine kurulur.
  *
  *  KUMANDA KANALLARI (EdgeTX, Mode 2, AETR) - argexika ile AYNI
  *  ------------------------------------------------------------------
  *  CH1 DONUS | CH2 ILERI/GERI | CH3 TILT | CH4 PAN | CH5 ARM/MOD
  *  CH6 HIZ LIMITI | CH7 TARET MERKEZ | CH10 ATES (yalniz LAZER modu)
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "crsf.h"
#include "reactor.h"
#include "servo.h"
#include "haberlesme.h"      /* STM <-> Jetson (USART2 / PA2-PA3, VCP) */
#include <string.h>

/* ==========================================================================
 *  AYARLANABILIR PARAMETRELER  (argexika ile birebir ayni)
 * ========================================================================== */

/* --- Kanal atamalari --- */
#define CH_STEER            1U      /* donus */
#define CH_THROTTLE         2U      /* ileri/geri */
#define CH_TILT             3U      /* dikey eksen servo */
#define CH_PAN              4U      /* yatay eksen servo */
#define CH_ARM              5U      /* silahlandirma anahtari (armed=SURUS, disarmed=LAZER) */
#define CH_SPEED_MODE       6U      /* 3 pozisyon hiz limiti */
#define CH_TURRET_CENTER    7U      /* servolari merkeze al */
#define CH_FIRE             10U     /* lazer atesleme tetigi (yalniz LAZER modu) */
#define FIRE_THRESH_US      1700U   /* CH10 bu esigin ustunde -> ates */

/* --- Surus davranisi --- */
#define STICK_DEADBAND_US   25U     /* stick merkez olu bandi */
#define TURN_GAIN_PCT       60      /* donus keskinligi (%). 100 = tam diferansiyel */
#define DRIVE_MAX_PCT_LOW   30      /* CH6 asagi  : ilk testler icin */
#define DRIVE_MAX_PCT_MID   60      /* CH6 orta   */
#define DRIVE_MAX_PCT_HIGH  100     /* CH6 yukari */

/* Motor yon terslemeleri: tekerlekler ters donerse -1 yapin. Once havada test edin! */
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

/* PAN "rate" modu (stick merkeze donen eksen -> hiz komutu) */
#define PAN_RATE_MODE       1
#define PAN_RATE_US_PER_S   400     /* tam stick -> saniyede 400us hareket */

/* TILT mutlak esleme (tirtikli throttle stick) */
#define TILT_RATE_MODE      0
#define TILT_RATE_US_PER_S  300

/* --- Zamanlama / failsafe --- */
#define LOOP_PERIOD_MS      10U     /* 100 Hz ana dongu */
#define CRSF_TIMEOUT_MS     300U    /* bu sure sinyal yoksa FAILSAFE */
#define CRSF_RX_BUF_SIZE    256U

/* ==========================================================================
 *  Cikis pinleri (hepsi Port C: PC0 / PC1 / PC2 / PC3)
 * ========================================================================== */
#define EN_PORT       GPIOC
#define EN_PIN        GPIO_PIN_0    /* PC0: Reactor EN (enable / role) */
#define FIRE_PORT     GPIOC
#define FIRE_PIN      GPIO_PIN_3    /* PC3: Lazer atesleme cikisi (HIGH = ates) */
#define LED_FS_PORT   GPIOC
#define LED_FS_PIN    GPIO_PIN_1    /* PC1: Kirmizi LED = FAILSAFE */
#define LED_OK_PORT   GPIOC
#define LED_OK_PIN    GPIO_PIN_2    /* PC2: Mavi LED = LINK OK */

/* ==========================================================================
 *  Global degiskenler
 * ========================================================================== */
UART_HandleTypeDef huart_crsf;    /* USART1 - CRSF alici (PA10 RX)  [DMA1 Ch3] */
UART_HandleTypeDef huart_left;    /* USART3 - SOL Reactor (PB10 TX)            */
UART_HandleTypeDef huart_right;   /* USART4 - SAG Reactor (PC10 TX)            */
DMA_HandleTypeDef  hdma_crsf_rx;  /* DMA1 Channel3 (USART1_RX). it.c'den erisilir */
TIM_HandleTypeDef  htim_servo;    /* TIM3 - servo PWM (PB0/PB1)                */

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

/* Lazer/ates durumu (telemetri + FIRE cikisi). */
static uint8_t   g_laser_mode = 0;   /* 0 = surus, 1 = lazer modu (CH5 disarmed) */
static uint8_t   g_fire_on    = 0;   /* 0/1 lazer atesleme cikisi */

/* ==========================================================================
 *  Fonksiyon prototipleri
 * ========================================================================== */
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_USART1_CRSF_Init(void);
static void MX_USART3_Left_Init(void);
static void MX_USART4_Right_Init(void);
static void MX_TIM3_Servo_Init(void);
void Error_Handler(void);

static void CRSF_PumpDMA(void);
static void Drive_Update(uint32_t now_ms, uint8_t link_ok);
static void Turret_Update(uint32_t now_ms, uint8_t link_ok);
static void Laser_Update(uint32_t now_ms, uint8_t link_ok);
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
    MX_USART1_CRSF_Init();
    MX_USART3_Left_Init();
    MX_USART4_Right_Init();
    MX_TIM3_Servo_Init();

    /* --- Modulleri baslat --- */
    CRSF_Init(&g_crsf);
    Reactor_Init(&g_left,  &huart_left,  LEFT_INV_A,  LEFT_INV_B);
    Reactor_Init(&g_right, &huart_right, RIGHT_INV_A, RIGHT_INV_B);

    Servo_Init(&g_pan,  &htim_servo, TIM_CHANNEL_3, PAN_MIN_US,  PAN_MAX_US,  PAN_CENTER_US);
    Servo_Init(&g_tilt, &htim_servo, TIM_CHANNEL_4, TILT_MIN_US, TILT_MAX_US, TILT_CENTER_US);

    /* Guvenlik: acilista motorlar kesinlikle dursun, lazer kapali olsun */
    Reactor_Stop(&g_left);
    Reactor_Stop(&g_right);
    HAL_GPIO_WritePin(EN_PORT,   EN_PIN,   GPIO_PIN_RESET);   /* EN = pasif    */
    HAL_GPIO_WritePin(FIRE_PORT, FIRE_PIN, GPIO_PIN_RESET);   /* ATES = kapali */

    /* Jetson telemetri/komut hattini baslat (USART2 / PA2-PA3 -> ST-Link VCP) */
    Haberlesme_Init();

    /* CRSF alimini dairesel DMA ile baslat */
    if (HAL_UART_Receive_DMA(&huart_crsf, g_rx_buf, CRSF_RX_BUF_SIZE) != HAL_OK)
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

        /* 3) Cikislari guncelle */
        Drive_Update(now, link_ok);
        Turret_Update(now, link_ok);
        Laser_Update(now, link_ok);

        /* 4) Durum LED'leri */
        HAL_GPIO_WritePin(LED_FS_PORT, LED_FS_PIN, link_ok ? GPIO_PIN_RESET : GPIO_PIN_SET);
        HAL_GPIO_WritePin(LED_OK_PORT, LED_OK_PIN, link_ok ? GPIO_PIN_SET   : GPIO_PIN_RESET);

        /* 5) Jetson telemetrisi (STATUS 20 Hz + HEARTBEAT 10 Hz) */
        Telemetry_Update(now, link_ok);
    }
}

/* ==========================================================================
 *  CRSF: DMA halka tamponundan byte cekip parser'a besle (argexika ile ayni)
 * ========================================================================== */
static void CRSF_PumpDMA(void)
{
    /* DMA hata nedeniyle durduysa yeniden baslat (ORE/NE/FE korumasi) */
    if (huart_crsf.RxState != HAL_UART_STATE_BUSY_RX)
    {
        __HAL_UART_CLEAR_OREFLAG(&huart_crsf);
        HAL_UART_Receive_DMA(&huart_crsf, g_rx_buf, CRSF_RX_BUF_SIZE);
        g_rx_tail = 0;
        return;
    }

    uint16_t head = (uint16_t)(CRSF_RX_BUF_SIZE - __HAL_DMA_GET_COUNTER(huart_crsf.hdmarx));
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
 *  SURUS: tank (skid-steer) mix -> iki Reactor surucusu (argexika ile ayni)
 * ========================================================================== */
static void Drive_Update(uint32_t now_ms, uint8_t link_ok)
{
    (void)now_ms;

    /* --- FAILSAFE: sinyal yok -> her sey dursun --- */
    if (!link_ok)
    {
        Reactor_Stop(&g_left);
        Reactor_Stop(&g_right);
        HAL_GPIO_WritePin(EN_PORT, EN_PIN, GPIO_PIN_RESET);
        g_applied_left  = 0;
        g_applied_right = 0;
        return;
    }

    /* --- ARM anahtari --- */
    uint8_t armed = (CRSF_GetChannelUs(&g_crsf, CH_ARM) > 1700U) ? 1U : 0U;
    HAL_GPIO_WritePin(EN_PORT, EN_PIN, armed ? GPIO_PIN_SET : GPIO_PIN_RESET);

    if (!armed)
    {
        Reactor_Stop(&g_left);
        Reactor_Stop(&g_right);
        g_applied_left  = 0;
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

    Reactor_SetSpeed(&g_left,  (int16_t)left,  (int16_t)left);
    Reactor_SetSpeed(&g_right, (int16_t)right, (int16_t)right);

    g_applied_left  = (int16_t)left;
    g_applied_right = (int16_t)right;
}

/* ==========================================================================
 *  TARET: pan (yatay) / tilt (dikey) servolari (argexika ile ayni)
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
 *  LAZER / ATES: CH5 disarmed -> LAZER modu; CH10 -> atesleme (argexika ile ayni)
 * ========================================================================== */
static void Laser_Update(uint32_t now_ms, uint8_t link_ok)
{
    (void)now_ms;

    /* FAILSAFE: her sey guvenli tarafa */
    if (!link_ok)
    {
        HAL_GPIO_WritePin(FIRE_PORT, FIRE_PIN, GPIO_PIN_RESET);
        g_fire_on    = 0U;
        g_laser_mode = 0U;
        return;
    }

    /* ARM anahtari: armed -> SURUS, disarmed -> LAZER */
    uint8_t armed = (CRSF_GetChannelUs(&g_crsf, CH_ARM) > 1700U) ? 1U : 0U;

    if (armed)
    {
        /* SURUS modu: atesleme kilitli */
        g_laser_mode = 0U;
        g_fire_on    = 0U;
        HAL_GPIO_WritePin(FIRE_PORT, FIRE_PIN, GPIO_PIN_RESET);
        return;
    }

    /* LAZER modu: CH10 tetigi ile atesle */
    g_laser_mode = 1U;
    uint8_t fire = (CRSF_GetChannelUs(&g_crsf, CH_FIRE) > FIRE_THRESH_US) ? 1U : 0U;
    g_fire_on    = fire;
    HAL_GPIO_WritePin(FIRE_PORT, FIRE_PIN, fire ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

/* ==========================================================================
 *  TELEMETRI: Jetson'a STATUS (20 Hz) + HEARTBEAT (10 Hz) (argexika ile ayni)
 * ========================================================================== */
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
        st.lazer    = g_fire_on;
        st.aktifMod = g_laser_mode;
        st.elrsLink = link_ok ? 1U : 0U;

        uint8_t durum = link_ok ? 0U : ST_FAILSAFE;
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

/* ==========================================================================
 *  HAL / donanim init fonksiyonlari (F072)
 * ========================================================================== */

void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    /* Dahili 48 MHz RC (HSI48) - Nucleo'da harici kristal/parca gerekmez. */
    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI48;
    RCC_OscInitStruct.HSI48State     = RCC_HSI48_ON;
    RCC_OscInitStruct.PLL.PLLState   = RCC_PLL_NONE;
    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
    {
        Error_Handler();
    }

    /* SYSCLK = HCLK = PCLK1 = 48 MHz  (F0'da tek APB vardir) */
    RCC_ClkInitStruct.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                                       RCC_CLOCKTYPE_PCLK1;
    RCC_ClkInitStruct.SYSCLKSource   = RCC_SYSCLKSOURCE_HSI48;
    RCC_ClkInitStruct.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_1) != HAL_OK)
    {
        Error_Handler();
    }
}

/* --- USART1 : CRSF alici, 420000, PA10 RX (AF1) + DMA1 Channel3 --- */
static void MX_USART1_CRSF_Init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_USART1_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();

    gpio.Pin       = GPIO_PIN_10;                /* USART1_RX */
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_PULLUP;                /* bosta HIGH -> sahte kenar olmasin */
    gpio.Speed     = GPIO_SPEED_FREQ_HIGH;
    gpio.Alternate = GPIO_AF1_USART1;
    HAL_GPIO_Init(GPIOA, &gpio);

    huart_crsf.Instance                    = USART1;
    huart_crsf.Init.BaudRate               = CRSF_BAUDRATE;
    huart_crsf.Init.WordLength             = UART_WORDLENGTH_8B;
    huart_crsf.Init.StopBits               = UART_STOPBITS_1;
    huart_crsf.Init.Parity                 = UART_PARITY_NONE;
    huart_crsf.Init.Mode                   = UART_MODE_RX;
    huart_crsf.Init.HwFlowCtl              = UART_HWCONTROL_NONE;
    huart_crsf.Init.OverSampling           = UART_OVERSAMPLING_16;
    huart_crsf.Init.OneBitSampling         = UART_ONE_BIT_SAMPLE_DISABLE;
    huart_crsf.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
    if (HAL_UART_Init(&huart_crsf) != HAL_OK)
    {
        Error_Handler();
    }

    /* USART1_RX -> DMA1 Channel3, dairesel (F0'da .Channel/.FIFOMode YOK) */
    hdma_crsf_rx.Instance                 = DMA1_Channel3;
    hdma_crsf_rx.Init.Direction           = DMA_PERIPH_TO_MEMORY;
    hdma_crsf_rx.Init.PeriphInc           = DMA_PINC_DISABLE;
    hdma_crsf_rx.Init.MemInc              = DMA_MINC_ENABLE;
    hdma_crsf_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    hdma_crsf_rx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
    hdma_crsf_rx.Init.Mode                = DMA_CIRCULAR;
    hdma_crsf_rx.Init.Priority            = DMA_PRIORITY_LOW;
    if (HAL_DMA_Init(&hdma_crsf_rx) != HAL_OK)
    {
        Error_Handler();
    }
    __HAL_LINKDMA(&huart_crsf, hdmarx, hdma_crsf_rx);
}

/* --- USART3 : SOL Reactor, 38400, PB10 TX (AF4) --- */
static void MX_USART3_Left_Init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_USART3_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    gpio.Pin       = GPIO_PIN_10;                /* USART3_TX */
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_NOPULL;
    gpio.Speed     = GPIO_SPEED_FREQ_HIGH;
    gpio.Alternate = GPIO_AF4_USART3;
    HAL_GPIO_Init(GPIOB, &gpio);

    huart_left.Instance                    = USART3;
    huart_left.Init.BaudRate               = REACTOR_BAUDRATE;
    huart_left.Init.WordLength             = UART_WORDLENGTH_8B;
    huart_left.Init.StopBits               = UART_STOPBITS_1;
    huart_left.Init.Parity                 = UART_PARITY_NONE;
    huart_left.Init.Mode                   = UART_MODE_TX;
    huart_left.Init.HwFlowCtl              = UART_HWCONTROL_NONE;
    huart_left.Init.OverSampling           = UART_OVERSAMPLING_16;
    huart_left.Init.OneBitSampling         = UART_ONE_BIT_SAMPLE_DISABLE;
    huart_left.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
    if (HAL_UART_Init(&huart_left) != HAL_OK)
    {
        Error_Handler();
    }
}

/* --- USART4 : SAG Reactor, 38400, PC10 TX (AF0) --- */
static void MX_USART4_Right_Init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_USART4_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    gpio.Pin       = GPIO_PIN_10;                /* USART4_TX */
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_NOPULL;
    gpio.Speed     = GPIO_SPEED_FREQ_HIGH;
    gpio.Alternate = GPIO_AF0_USART4;
    HAL_GPIO_Init(GPIOC, &gpio);

    huart_right.Instance                    = USART4;
    huart_right.Init.BaudRate               = REACTOR_BAUDRATE;
    huart_right.Init.WordLength             = UART_WORDLENGTH_8B;
    huart_right.Init.StopBits               = UART_STOPBITS_1;
    huart_right.Init.Parity                 = UART_PARITY_NONE;
    huart_right.Init.Mode                   = UART_MODE_TX;
    huart_right.Init.HwFlowCtl              = UART_HWCONTROL_NONE;
    huart_right.Init.OverSampling           = UART_OVERSAMPLING_16;
    huart_right.Init.OneBitSampling         = UART_ONE_BIT_SAMPLE_DISABLE;
    huart_right.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
    if (HAL_UART_Init(&huart_right) != HAL_OK)
    {
        Error_Handler();
    }
}

/* --- TIM3 : 50 Hz servo PWM, 1 us cozunurluk, PB0/PB1 (AF1) --- */
static void MX_TIM3_Servo_Init(void)
{
    TIM_ClockConfigTypeDef  sClockSourceConfig = {0};
    TIM_MasterConfigTypeDef sMasterConfig      = {0};
    TIM_OC_InitTypeDef      sConfigOC          = {0};
    GPIO_InitTypeDef        gpio               = {0};

    __HAL_RCC_TIM3_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    htim_servo.Instance               = TIM3;
    htim_servo.Init.Prescaler         = 48 - 1;      /* 48 MHz / 48 = 1 MHz   */
    htim_servo.Init.CounterMode       = TIM_COUNTERMODE_UP;
    htim_servo.Init.Period            = 20000 - 1;   /* 1 MHz / 20000 = 50 Hz */
    htim_servo.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    htim_servo.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
    if (HAL_TIM_Base_Init(&htim_servo) != HAL_OK)
    {
        Error_Handler();
    }

    sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
    if (HAL_TIM_ConfigClockSource(&htim_servo, &sClockSourceConfig) != HAL_OK)
    {
        Error_Handler();
    }

    if (HAL_TIM_PWM_Init(&htim_servo) != HAL_OK)
    {
        Error_Handler();
    }

    sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
    sMasterConfig.MasterSlaveMode     = TIM_MASTERSLAVEMODE_DISABLE;
    if (HAL_TIMEx_MasterConfigSynchronization(&htim_servo, &sMasterConfig) != HAL_OK)
    {
        Error_Handler();
    }

    sConfigOC.OCMode     = TIM_OCMODE_PWM1;
    sConfigOC.Pulse      = SERVO_MID_US;
    sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
    sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
    if (HAL_TIM_PWM_ConfigChannel(&htim_servo, &sConfigOC, TIM_CHANNEL_3) != HAL_OK)
    {
        Error_Handler();
    }
    if (HAL_TIM_PWM_ConfigChannel(&htim_servo, &sConfigOC, TIM_CHANNEL_4) != HAL_OK)
    {
        Error_Handler();
    }

    /* PB0 -> TIM3_CH3 (PAN) , PB1 -> TIM3_CH4 (TILT) , ikisi de AF1 */
    gpio.Pin       = GPIO_PIN_0 | GPIO_PIN_1;
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_NOPULL;
    gpio.Speed     = GPIO_SPEED_FREQ_LOW;
    gpio.Alternate = GPIO_AF1_TIM3;
    HAL_GPIO_Init(GPIOB, &gpio);
}

/* --- DMA : DMA1 clock + CRSF (Ch3) icin NVIC --- */
static void MX_DMA_Init(void)
{
    __HAL_RCC_DMA1_CLK_ENABLE();

    /* Ch2/Ch3 ortak vektor: CRSF (USART1_RX) Channel3'te. M0 oncelik 0..3. */
    HAL_NVIC_SetPriority(DMA1_Channel2_3_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(DMA1_Channel2_3_IRQn);
    /* Jetson RX (Ch5, DMA1_Channel4_5_6_7_IRQn) NVIC'i haberlesme.c'de acilir. */
}

/* --- GPIO : Port C cikislari (EN, FIRE, LED'ler) --- */
static void MX_GPIO_Init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOC_CLK_ENABLE();

    /* Hepsini once guvenli LOW'a cek */
    HAL_GPIO_WritePin(GPIOC, EN_PIN | FIRE_PIN | LED_FS_PIN | LED_OK_PIN, GPIO_PIN_RESET);

    gpio.Pin   = EN_PIN | FIRE_PIN | LED_FS_PIN | LED_OK_PIN;   /* PC0,PC3,PC1,PC2 */
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOC, &gpio);
}

/* --- Hata yakalayici: motorlari durdurup bekle --- */
void Error_Handler(void)
{
    __disable_irq();
    while (1)
    {
        /* PC1 kirmizi LED yanik kalir (FAILSAFE gostergesi) */
        HAL_GPIO_WritePin(LED_FS_PORT, LED_FS_PIN, GPIO_PIN_SET);
    }
}
