/**
  ******************************************************************************
  * @file    main.c
  * @brief   TEKNOFEST IKA - Nucleo-F072RB portu (argexika/F407 "lazerson" firmware)
  *          RadioMaster TX12 MKII (ELRS/CRSF) -> 2x Reactor motor surucu (UART)
  *                                            -> 2x RDS3235 servo (pan/tilt)
  *                                            -> Jetson: NATIVE USB-CDC (PA11/PA12)
  *
  *  Uygulama mantigi (mod/E-STOP/lazer/otonom/telemetri) argexika (F407) ile
  *  BIREBIR AYNI. Yalniz donanim katmani (clock, USART/DMA/TIM/GPIO/USB) F072'ye
  *  tasindi. Ayrinti: nucleo-f072/KURULUM.md
  *
  *  PIN HARITASI (F072RB, LQFP64)
  *  ------------------------------------------------------------------
  *  PA11  USB_DM  } native USB Full-Speed CDC  <-> Jetson (/dev/ttyACM*)
  *  PA12  USB_DP  }   (haricen bir USB konnektore kablolanir; onboard USB = ST-Link)
  *  PA10  USART1_RX  420000  <-  ELRS alici TX pedi (CRSF)   [DMA1 Channel3]
  *  PB10  USART3_TX   38400  ->  SOL Reactor "SRL" pini
  *  PC10  USART4_TX   38400  ->  SAG Reactor "SRL" pini
  *  PB0   TIM3_CH3    50 Hz  ->  PAN  (yatay) RDS3235 sinyal
  *  PB1   TIM3_CH4    50 Hz  ->  TILT (dikey) RDS3235 sinyal
  *  PC0   GPIO out           ->  Reactor EN pini / role surucu
  *  PC3   GPIO out           ->  LAZER atesleme cikisi (HIGH = ates)
  *  PC1   GPIO out           ->  Kirmizi LED : FAILSAFE
  *  PC2   GPIO out           ->  Mavi LED    : LINK OK
  *
  *  KUMANDA KANALLARI (CRSF, 1-tabanli) - argexika lazerson ile AYNI
  *  ------------------------------------------------------------------
  *  CH1/CH2 PAYLASIMLI: SURUS modunda motor (donus/gaz), LAZER modunda taret (pan/tilt)
  *  CH5 MANUEL/OTONOM | CH6 HIZ LIMITI | CH7 LAZER modu | CH9 E-STOP | CH10 ATES
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "usb_device.h"      /* MX_USB_DEVICE_Init() (CubeMX uretir) */
#include "crsf.h"
#include "reactor.h"
#include "servo.h"
#include "haberlesme.h"      /* STM <-> Jetson : native USB-CDC (PA11/PA12) */
#include <string.h>

/* ==========================================================================
 *  AYARLANABILIR PARAMETRELER  (argexika lazerson ile birebir ayni)
 * ========================================================================== */

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

/* ==========================================================================
 *  Cikis pinleri (hepsi Port C: PC0 / PC1 / PC2 / PC3)
 *  F407'de: EN=PE0, FIRE=PE1, LED kirmizi=PD14, LED mavi=PD15 idi.
 *  F072RB'de Port D/E yok -> hepsi Port C'ye tasindi.
 * ========================================================================== */
#define EN_PORT       GPIOC
#define EN_PIN        GPIO_PIN_0    /* PC0: Reactor EN (enable / role) */
#define FIRE_PORT     GPIOC
#define FIRE_PIN      GPIO_PIN_3    /* PC3: Lazer atesleme cikisi (HIGH = ates) */
#define LED_FS_PORT   GPIOC
#define LED_FS_PIN    GPIO_PIN_1    /* PC1: Kirmizi LED = FAILSAFE (link yok/E-STOP) */
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

/* Mod/durum (Control_Decide her dongu gunceller). */
static uint8_t   g_laser_mode  = 0;  /* 0 = surus, 1 = lazer modu (CH7) */
static uint8_t   g_auto_active = 0;  /* 0 = manuel, 1 = otonom (CH5) -> telemetri ST_AUTO_EN */
static uint8_t   g_estop       = 0;  /* 1 = kilitli acil durdurma (CH9 buton) */
static uint8_t   g_fire_on     = 0;  /* 0/1 lazer atesleme cikisi (FIRE_PIN) */

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

static void CRSF_PumpDMA(void);
static void Control_Decide(uint32_t now_ms, uint8_t link_ok);
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
  MX_USB_DEVICE_Init();      /* native USB-CDC (PA11/PA12) - CubeMX uretir */

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

  /* Jetson telemetri hattini baslat (native USB-CDC; donanim MX_USB_DEVICE_Init'te) */
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

    /* 1b) Jetson -> STM COMMAND/HEARTBEAT: USB-CDC halka tamponunu her spin
           bosalt (non-blocking). Gelen komut JetsonKomut'a yazilir. */
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
 *  MOD / E-STOP KARARI  (argexika lazerson ile birebir ayni)
 *   - CH9 anlik buton -> kilitli E-STOP (bir bas: her sey durur+kilitlenir;
 *     reset: butonu birak + CH5'i MANUEL konuma al = operator kontrolu geri aldi)
 *   - CH7 > esik  -> LAZER modu (ONCELIKLI; motorlar kilitlenir)
 *   - CH5 > esik  -> OTONOM, degilse MANUEL (yalniz lazer kapaliyken gecerli)
 * ========================================================================== */
static void Control_Decide(uint32_t now_ms, uint8_t link_ok)
{
  (void)now_ms;
  static uint8_t prev_btn = 0U;

  /* Link yoksa: mod bayraklari guvenli varsayilana. E-STOP mandali korunur. */
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
 *  SURUS: tank (skid-steer) mix -> iki Reactor surucusu (argexika ile ayni)
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
    HAL_GPIO_WritePin(EN_PORT, EN_PIN, GPIO_PIN_RESET);   /* EN pasif */
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
      HAL_GPIO_WritePin(EN_PORT, EN_PIN, GPIO_PIN_SET);
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
    HAL_GPIO_WritePin(EN_PORT, EN_PIN, GPIO_PIN_RESET);
    g_applied_left  = 0;
    g_applied_right = 0;
    return;
  }

  /* --- MANUEL surus: RC stickleri (CH1 donus, CH2 gaz) --- */
  HAL_GPIO_WritePin(EN_PORT, EN_PIN, GPIO_PIN_SET);   /* EN aktif */

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
 *  TARET: pan (yatay) / tilt (dikey) servolari (argexika lazerson ile ayni)
 *   Taret YALNIZ lazer modunda kumanda edilir; stickler CH1 (pan) / CH2 (tilt).
 *   Diger modlarda (surus) servolar SON KONUMDA kalir.
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
 *  LAZER ATES: yalniz LAZER modunda (CH7) CH10 tetigi ile FIRE_PIN -> HIGH.
 *   Lazer modu disinda / E-STOP / link yok -> ates kapali. (argexika ile ayni)
 * ========================================================================== */
static void Laser_Update(uint32_t now_ms, uint8_t link_ok)
{
  (void)now_ms;

  if (!link_ok || g_estop || !g_laser_mode)
  {
    HAL_GPIO_WritePin(FIRE_PORT, FIRE_PIN, GPIO_PIN_RESET);
    g_fire_on = 0U;
    return;
  }

  /* LAZER modu: CH10 tetigi ile atesle */
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
    st.lazer    = g_fire_on;                        /* CH10 atesleme durumu */
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

/* ==========================================================================
 *  HAL / donanim init fonksiyonlari (F072)
 * ========================================================================== */

/**
  * @brief  Sistem saati: dahili HSI48 -> 48 MHz. USB clock = HSI48, CRS ile
  *         USB SOF'a kilitlenir (FS icin gereken hassasiyet). Harici kristal yok.
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef       RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef       RCC_ClkInitStruct = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInit     = {0};
  RCC_CRSInitTypeDef       CrsInit           = {0};

  /* Dahili 48 MHz RC (HSI48) */
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

  /* USB peripheral clock = HSI48 */
  PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_USB;
  PeriphClkInit.UsbClockSelection    = RCC_USBCLKSOURCE_HSI48;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
  {
    Error_Handler();
  }

  /* CRS: HSI48'i USB SOF cercevelerine kilitle (kristalsiz USB enumerasyonu icin sart) */
  __HAL_RCC_CRS_CLK_ENABLE();
  CrsInit.Prescaler             = RCC_CRS_SYNC_DIV1;
  CrsInit.Source                = RCC_CRS_SYNC_SOURCE_USB;
  CrsInit.Polarity              = RCC_CRS_SYNC_POLARITY_RISING;
  CrsInit.ReloadValue           = __HAL_RCC_CRS_RELOADVALUE_CALCULATE(48000000U, 1000U);
  CrsInit.ErrorLimitValue       = RCC_CRS_ERRORLIMIT_DEFAULT;
  CrsInit.HSI48CalibrationValue = RCC_CRS_HSI48CALIBRATION_DEFAULT;
  HAL_RCCEx_CRSConfig(&CrsInit);
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

/* --- Hata yakalayici: kesmeleri kapat, kirmizi LED yanik kalir --- */
void Error_Handler(void)
{
  __disable_irq();
  HAL_GPIO_WritePin(LED_FS_PORT, LED_FS_PIN, GPIO_PIN_SET);   /* PC1 kirmizi LED */
  while (1)
  {
  }
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
  (void)file;
  (void)line;
}
#endif /* USE_FULL_ASSERT */
