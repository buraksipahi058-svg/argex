// IKA ESP32 firmware. Kurulum ve sinirlar icin ../README_TR.md
#include <Arduino.h>

#include <esp_arduino_version.h>
#include <esp32-hal-rmt.h>
#include "config.h"
#include "motion.h"
#if IMU_ENABLED
#include <Wire.h>              /* BNO055 IMU (sadece telemetri) */
#endif
#if !defined(CONFIG_IDF_TARGET_ESP32) || ESP_ARDUINO_VERSION_MAJOR < 3
#error "ESP32 Dev Module (WROOM-32) ve esp32 by Espressif Systems 3.3.0 secin."
#endif
static bool g_hw_fault = false;
/* ==========================================================================
 * PINLER
 * ========================================================================== */
















/* ==========================================================================
 * UART / ZAMAN
 * ========================================================================== */
#define CRSF_BAUD                 420000UL
#define REACTOR_BAUD               38400UL
#define JETSON_BAUD               115200UL
#define DEBUG_BAUD                115200UL

#define LOOP_PERIOD_MS                10UL
#define CRSF_TIMEOUT_MS              300UL
#define STATUS_PERIOD_MS              50UL
#define HB_PERIOD_MS                 100UL
#define CMD_TIMEOUT_MS               200UL
#define JETSON_HB_TIMEOUT_MS         500UL
#define DEBUG_PERIOD_MS              500UL
#define IMU_PERIOD_MS                 50UL   /* BNO055 pitch/yaw gonderim periyodu */

#define STARTUP_HOLD_MS             1200UL
#define ARM_STABLE_MS                500UL
#define MANUAL_ARM_CENTER_NORM        120




#define DEBUG_ENABLED                  1

HardwareSerial JetsonSerial(1);  /* UART1 -> GPIO21/22 */
/* Serial2 -> CRSF */
/* Serial  -> USB Serial Monitor */

/* ==========================================================================
 * KUMANDA
 * ========================================================================== */
#define CH_STEER                    1
#define CH_THROTTLE                 2
#define CH_MANUAL_SERVO             5
#define CH_SPEED_MODE               6
#define CH_AUTO                     8
#define CH_FIRE                    10

#define STICK_DEADBAND_US           25
#define TURRET_DEADBAND_US          50

#define MODE_HIGH_US              1700U
#define AUTO_SELECT_NORM           800







/* ==========================================================================
 * MOTOR YONLERI
 * ========================================================================== */





/* ==========================================================================
 * SERVO
 * ========================================================================== */
#define SERVO_FREQ_HZ                50
#define SERVO_PWM_RES_BITS           16









/* Mevcut mekanikte iki servo yonu de ters oldugu icin -1 korunuyor. */






#if ESP_ARDUINO_VERSION_MAJOR < 3
static const uint8_t PAN_LEDC_CH  = 4;
static const uint8_t TILT_LEDC_CH = 5;
#endif

/* ==========================================================================
 * CRSF / ELRS COZUCU
 * ========================================================================== */
#define CRSF_MAX_CHANNELS          16
#define CRSF_SYNC_BYTE           0xC8
#define CRSF_SYNC_BYTE_ALT       0xEE
#define CRSF_TYPE_RC             0x16
#define CRSF_BUF_SIZE              64

#define CRSF_US_MID              1500U
#define CRSF_US_MAX              2012U

enum {
  CRSF_WAIT_SYNC = 0,
  CRSF_WAIT_LEN  = 1,
  CRSF_PAYLOAD   = 2
};

struct CrsfState {
  uint16_t channel_us[CRSF_MAX_CHANNELS];
  uint32_t last_frame_ms;
  uint32_t frame_count;
  uint32_t crc_error_count;
  uint8_t  state;
  uint8_t  len;
  uint8_t  idx;
  uint8_t  buf[CRSF_BUF_SIZE];
};

static CrsfState g_crsf;
static bool g_lq_seen = false;
static uint8_t g_lq = 100;

/* ==========================================================================
 * ARDUINO .INO ON-ISLEYICI ICIN ERKEN TIP TANIMLARI
 *
 * Arduino IDE .ino dosyalarinda fonksiyon prototiplerini otomatik uretir.
 * Bu tipler fonksiyonlardan sonra tanimli olursa "does not name a type"
 * hatasi cikabilir. Bu nedenle tum fonksiyonlardan once tanimlidir.
 * ========================================================================== */
struct VehicleState {
  int8_t  solMotor;
  int8_t  sagMotor;
  uint8_t pan;
  uint8_t tilt;
  uint8_t lazer;
  uint8_t aktifMod;
  uint8_t elrsLink;
  uint8_t durum;
};

struct JetsonCommand {
  int8_t   solHedef;
  int8_t   sagHedef;
  uint8_t  panHedef;
  uint8_t  tiltHedef;
  uint8_t  lazerKomut;
  uint8_t  modKomut;
  uint8_t  bayrak;
  uint32_t sonAlim;
};

enum VehicleMode : uint8_t {
  MODE_MANUAL_DRIVE  = 0,
  MODE_MANUAL_TURRET = 1,
  MODE_AUTONOMOUS    = 2
};

static uint8_t Crsf_Crc8(const uint8_t *p, uint8_t len) {
  uint8_t crc = 0;

  for (uint8_t i = 0; i < len; i++) {
    crc ^= p[i];

    for (uint8_t j = 0; j < 8; j++) {
      crc = (crc & 0x80U)
              ? (uint8_t)((crc << 1) ^ 0xD5U)
              : (uint8_t)(crc << 1);
    }
  }

  return crc;
}

static uint16_t Crsf_RawToUs(uint16_t raw) {
  return (uint16_t)(((uint32_t)raw * 5U) / 8U + 880U);
}

static void Crsf_UnpackChannels(CrsfState *c, const uint8_t *payload) {
  uint32_t bits = 0;
  uint8_t bits_avail = 0;
  uint8_t idx = 0;

  for (uint8_t ch = 0; ch < CRSF_MAX_CHANNELS; ch++) {
    while (bits_avail < 11U) {
      bits |= ((uint32_t)payload[idx++]) << bits_avail;
      bits_avail += 8U;
    }

    uint16_t raw = (uint16_t)(bits & 0x7FFU);
    bits >>= 11U;
    bits_avail -= 11U;

    c->channel_us[ch] = Crsf_RawToUs(raw);
  }
}

static void Crsf_Init(CrsfState *c) {
  for (uint8_t i = 0; i < CRSF_MAX_CHANNELS; i++) {
    c->channel_us[i] = CRSF_US_MID;
  }

  c->last_frame_ms   = 0;
  c->frame_count     = 0;
  c->crc_error_count = 0;
  c->state           = CRSF_WAIT_SYNC;
  c->len             = 0;
  c->idx             = 0;
}

static void Crsf_ParseByte(CrsfState *c, uint8_t b, uint32_t now_ms) {
  static uint32_t last_byte_ms = 0;
  if ((uint32_t)(now_ms - last_byte_ms) > 10) c->state = CRSF_WAIT_SYNC;
  last_byte_ms = now_ms;
  switch (c->state) {
    case CRSF_WAIT_SYNC:
      if ((b == CRSF_SYNC_BYTE) || (b == CRSF_SYNC_BYTE_ALT)) {
        c->state = CRSF_WAIT_LEN;
      }
      break;

    case CRSF_WAIT_LEN:
      if ((b >= 2U) && (b <= (CRSF_BUF_SIZE - 1U))) {
        c->len = b;
        c->idx = 0;
        c->state = CRSF_PAYLOAD;
      } else {
        c->state = CRSF_WAIT_SYNC;
      }
      break;

    case CRSF_PAYLOAD:
      c->buf[c->idx++] = b;

      if (c->idx >= c->len) {
        uint8_t crc_calc = Crsf_Crc8(c->buf, (uint8_t)(c->len - 1U));
        uint8_t crc_rx   = c->buf[c->len - 1U];

        if (crc_calc == crc_rx) {
          /* TYPE + 22 byte channel payload + CRC => len >= 24 */
          if ((c->buf[0] == CRSF_TYPE_RC) && (c->len == 24U)) {
            Crsf_UnpackChannels(c, &c->buf[1]);
            c->last_frame_ms = now_ms;
            c->frame_count++;
          } else if (c->buf[0] == 0x14 && c->len == 12) {
            g_lq = c->buf[3]; g_lq_seen = true;
          }
        } else {
          c->crc_error_count++;
        }

        c->state = CRSF_WAIT_SYNC;
      }
      break;

    default:
      c->state = CRSF_WAIT_SYNC;
      break;
  }
}

static bool Crsf_IsLinkUp(const CrsfState *c, uint32_t now_ms) {
  if (c->frame_count == 0U || (g_lq_seen && g_lq == 0)) {
    return false;
  }

  return ((uint32_t)(now_ms - c->last_frame_ms) < CRSF_TIMEOUT_MS);
}

static uint16_t Crsf_GetUs(const CrsfState *c, uint8_t ch_1based) {
  if ((ch_1based < 1U) || (ch_1based > CRSF_MAX_CHANNELS)) {
    return CRSF_US_MID;
  }

  return c->channel_us[ch_1based - 1U];
}

static int16_t Crsf_GetNorm(const CrsfState *c,
                            uint8_t ch_1based,
                            uint16_t deadband_us) {
  int32_t us = (int32_t)Crsf_GetUs(c, ch_1based);
  int32_t d  = us - (int32_t)CRSF_US_MID;

  if ((d > -(int32_t)deadband_us) &&
      (d <  (int32_t)deadband_us)) {
    return 0;
  }

  if (d > 0) {
    d -= (int32_t)deadband_us;
  } else {
    d += (int32_t)deadband_us;
  }

  int32_t span =
      (int32_t)(CRSF_US_MAX - CRSF_US_MID) -
      (int32_t)deadband_us;

  if (span < 1) {
    span = 1;
  }

  int32_t n = (d * 1000L) / span;

  if (n > 1000)  n = 1000;
  if (n < -1000) n = -1000;

  return (int16_t)n;
}

static void Crsf_Poll(uint32_t now_ms) {
  unsigned budget = 512;
  while (budget-- && Serial2.available() > 0) {
    int v = Serial2.read();
    if (v >= 0) {
      Crsf_ParseByte(&g_crsf, (uint8_t)v, now_ms);
    }
  }
}

/* ==========================================================================
 * REACTOR V1.2 UART SERIAL-IN
 * ========================================================================== */
#define REACTOR_A_STOP              64
#define REACTOR_B_STOP             192
#define REACTOR_HALF_SPAN           63

static bool g_rmt_left = false, g_rmt_right = false;
static void Reactor_Init() {
  g_rmt_left = rmtInit(REACTOR_LEFT_TX_PIN, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_1, 10000000);
  g_rmt_right = rmtInit(REACTOR_RIGHT_TX_PIN, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_1, 10000000);
  if (g_rmt_left) g_rmt_left = rmtSetEOT(REACTOR_LEFT_TX_PIN, HIGH);
  if (g_rmt_right) g_rmt_right = rmtSetEOT(REACTOR_RIGHT_TX_PIN, HIGH);
  if (!g_rmt_left || !g_rmt_right) g_hw_fault = true;
}

static int16_t Reactor_ClampSpeed(int32_t speed) {
  if (speed > 1000)  speed = 1000;
  if (speed < -1000) speed = -1000;
  return (int16_t)speed;
}

static uint8_t Reactor_ByteA(int16_t speed) {
  int32_t v =
      (int32_t)REACTOR_A_STOP +
      ((int32_t)speed * REACTOR_HALF_SPAN) / 1000L;

  if (v < 1)   v = 1;
  if (v > 127) v = 127;

  return (uint8_t)v;
}

static uint8_t Reactor_ByteB(int16_t speed) {
  int32_t v =
      (int32_t)REACTOR_B_STOP +
      ((int32_t)speed * REACTOR_HALF_SPAN) / 1000L;

  if (v < 128) v = 128;
  if (v > 255) v = 255;

  return (uint8_t)v;
}

static void Reactor_Write2(uint8_t pin, uint8_t a, uint8_t b) {
  if ((pin == REACTOR_LEFT_TX_PIN && !g_rmt_left) ||
      (pin == REACTOR_RIGHT_TX_PIN && !g_rmt_right)) return;
  rmt_data_t symbols[10] = {};
  const uint8_t bytes[2] = {a,b};
  // 20 UART bits: two start + 16 data (LSB first) + two stop.
  // 10MHz tick; cumulative rounding gives 38400 baud within 0.1us.
  for (unsigned bit = 0; bit < 20; ++bit) {
    unsigned local = bit % 10;
    unsigned level = local == 0 ? 0 : local == 9 ? 1 : ((bytes[bit/10] >> (local-1)) & 1);
    unsigned ticks = ((bit+1)*10000000UL + REACTOR_BAUD/2)/REACTOR_BAUD
                   - (bit*10000000UL + REACTOR_BAUD/2)/REACTOR_BAUD;
    if (bit & 1) { symbols[bit/2].level1=level; symbols[bit/2].duration1=ticks; }
    else { symbols[bit/2].level0=level; symbols[bit/2].duration0=ticks; }
  }
  if (!rmtWrite(pin, symbols, 10, 5)) g_hw_fault = true;
}

static void Reactor_LeftSetSpeed(int16_t speed) {
  speed = Reactor_ClampSpeed(speed);

  uint8_t a = Reactor_ByteA(
      Reactor_ClampSpeed((int32_t)speed * INV_LEFT_A));

  uint8_t b = Reactor_ByteB(
      Reactor_ClampSpeed((int32_t)speed * INV_LEFT_B));

  Reactor_Write2(REACTOR_LEFT_TX_PIN, a, b);
}

static void Reactor_RightSetSpeed(int16_t speed) {
  speed = Reactor_ClampSpeed(speed);

  uint8_t a = Reactor_ByteA(
      Reactor_ClampSpeed((int32_t)speed * INV_RIGHT_A));

  uint8_t b = Reactor_ByteB(
      Reactor_ClampSpeed((int32_t)speed * INV_RIGHT_B));

  Reactor_Write2(REACTOR_RIGHT_TX_PIN, a, b);
}

static void Reactor_SetDrive(int16_t left_speed, int16_t right_speed) {
  Reactor_LeftSetSpeed(left_speed);
  Reactor_RightSetSpeed(right_speed);
}

static void Reactor_StopAll(void) {
  Reactor_Write2(
      REACTOR_LEFT_TX_PIN,
      REACTOR_A_STOP,
      REACTOR_B_STOP);

  Reactor_Write2(
      REACTOR_RIGHT_TX_PIN,
      REACTOR_A_STOP,
      REACTOR_B_STOP);
}

static void Reactor_Enable(bool enable) {
  if (REACTOR_EN_PIN < 0) return;
  if (g_hw_fault) enable = false;
#if REACTOR_EN_ACTIVE_HIGH
  digitalWrite(REACTOR_EN_PIN, enable ? HIGH : LOW);
#else
  digitalWrite(REACTOR_EN_PIN, enable ? LOW : HIGH);
#endif
}

/* ==========================================================================
 * SERVO
 * ========================================================================== */
static int32_t g_pan_us  = PAN_CENTER_US;
static int32_t g_tilt_us = TILT_CENTER_US;

static int32_t g_pan_rate_accum  = 0;
static int32_t g_tilt_rate_accum = 0;
static ServoInputFilter g_pan_filter, g_tilt_filter;
static uint32_t g_servo_update_ms=0;
static void Servo_ResetInputFilters() {
  g_pan_filter.reset();g_tilt_filter.reset();
  g_pan_rate_accum=g_tilt_rate_accum=0;
  g_servo_update_ms=millis();
}

static int32_t Servo_ClampUs(int32_t us, int32_t min_us, int32_t max_us) {
  if (us < min_us) us = min_us;
  if (us > max_us) us = max_us;
  return us;
}

static uint32_t Servo_UsToDuty(uint32_t pulse_us) {
  return (uint32_t)(((uint64_t)pulse_us * 65535ULL) / 20000ULL);
}

static void ServoPwm_Init(void) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  if (!ledcAttach(SERVO_PAN_PIN, SERVO_FREQ_HZ, SERVO_PWM_RES_BITS)) {
    g_hw_fault = true; Serial.println("HATA: PAN LEDC attach olmadi.");
  }

  if (!ledcAttach(SERVO_TILT_PIN, SERVO_FREQ_HZ, SERVO_PWM_RES_BITS)) {
    g_hw_fault = true; Serial.println("HATA: TILT LEDC attach olmadi.");
  }
#else
  ledcSetup(PAN_LEDC_CH, SERVO_FREQ_HZ, SERVO_PWM_RES_BITS);
  ledcSetup(TILT_LEDC_CH, SERVO_FREQ_HZ, SERVO_PWM_RES_BITS);

  ledcAttachPin(SERVO_PAN_PIN, PAN_LEDC_CH);
  ledcAttachPin(SERVO_TILT_PIN, TILT_LEDC_CH);
#endif
}

static void ServoPan_WriteUs(int32_t us) {
  g_pan_us = Servo_ClampUs(us, PAN_MIN_US, PAN_MAX_US);
  uint32_t duty = Servo_UsToDuty((uint32_t)g_pan_us);
  static uint32_t last_duty=0;
  if(duty==last_duty)return;
  last_duty=duty;

#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(SERVO_PAN_PIN, duty);
#else
  ledcWrite(PAN_LEDC_CH, duty);
#endif
}

static void ServoTilt_WriteUs(int32_t us) {
  g_tilt_us = Servo_ClampUs(us, TILT_MIN_US, TILT_MAX_US);
  uint32_t duty = Servo_UsToDuty((uint32_t)g_tilt_us);
  static uint32_t last_duty=0;
  if(duty==last_duty)return;
  last_duty=duty;

#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(SERVO_TILT_PIN, duty);
#else
  ledcWrite(TILT_LEDC_CH, duty);
#endif
}

static void ServoPan_AddUs(int32_t delta_us) {
  ServoPan_WriteUs(g_pan_us + delta_us);
}

static void ServoTilt_AddUs(int32_t delta_us) {
  ServoTilt_WriteUs(g_tilt_us + delta_us);
}

static int32_t Servo_DegToUsCentered(int32_t min_us,
                                     int32_t center_us,
                                     int32_t max_us,
                                     uint8_t deg) {
  if (deg > 180U) {
    deg = 180U;
  }

  if (deg <= 90U) {
    int32_t span = center_us - min_us;
    return center_us - ((int32_t)(90U - deg) * span) / 90L;
  }

  int32_t span = max_us - center_us;
  return center_us + ((int32_t)(deg - 90U) * span) / 90L;
}

static uint8_t Servo_UsToDegCentered(int32_t pos_us,
                                     int32_t min_us,
                                     int32_t center_us,
                                     int32_t max_us) {
  if (pos_us <= center_us) {
    int32_t span = center_us - min_us;
    if (span <= 0) return 90U;

    int32_t deg =
        90L - ((center_us - pos_us) * 90L) / span;

    if (deg < 0) deg = 0;
    return (uint8_t)deg;
  }

  int32_t span = max_us - center_us;
  if (span <= 0) return 90U;

  int32_t deg =
      90L + ((pos_us - center_us) * 90L) / span;

  if (deg > 180) deg = 180;
  return (uint8_t)deg;
}

/* ==========================================================================
 * JETSON BINARY PROTOKOLU
 * ========================================================================== */
#define PROTO_HDR0                0xAAU
#define PROTO_HDR1                0x55U
#define PROTO_VERSION             0x01U
#define PROTO_MAX_PAYLOAD         32U

#define TYPE_STATUS               0x01U
#define TYPE_COMMAND              0x02U
#define TYPE_HEARTBEAT            0x03U
#define TYPE_IMU                  0x04U

#define HB_KAYNAK_CONTROLLER      0x00U
#define HB_KAYNAK_JETSON          0x01U

#define CMD_FLAG_AUTO_REQ         0x01U
#define MOD_KOMUT_DEGISTIRME      0xFFU

#define ST_JETSON_LINK            0x01U
#define ST_CMD_TIMEOUT            0x02U
#define ST_AUTO_EN                0x04U
#define ST_FAILSAFE               0x08U
#define ST_CRC_ERR                0x10U
#define ST_ARMED                  0x20U
#define ST_HW_ERROR               0x40U


static JetsonCommand g_cmd;
static bool g_cmd_received = false;
static bool g_jetson_seen  = false;
static uint32_t g_jetson_last_ms = 0;
static uint16_t g_rx_lost = 0;
static bool g_rx_crc_error = false;
static uint8_t g_tx_seq = 0;

enum {
  JRX_H0 = 0,
  JRX_H1,
  JRX_VER,
  JRX_TYP,
  JRX_LEN,
  JRX_SEQ,
  JRX_PAY,
  JRX_CRCL,
  JRX_CRCH
};

static uint8_t g_jrx_state = JRX_H0;
static uint8_t g_jrx_len = 0;
static uint8_t g_jrx_i = 0;
static uint8_t g_jrx_crc_lo = 0;
static uint8_t g_jrx_asm[4U + PROTO_MAX_PAYLOAD];

static bool g_jrx_seq_valid = false;
static uint8_t g_jrx_last_seq = 0;

static uint16_t Proto_Crc16(const uint8_t *data, uint16_t len) {
  uint16_t crc = 0xFFFFU;

  for (uint16_t i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i] << 8U;

    for (uint8_t b = 0; b < 8U; b++) {
      if ((crc & 0x8000U) != 0U) {
        crc = (uint16_t)((crc << 1U) ^ 0x1021U);
      } else {
        crc = (uint16_t)(crc << 1U);
      }
    }
  }

  return crc;
}

static void Proto_SendFrame(uint8_t type,
                            const uint8_t *payload,
                            uint8_t len) {
  if (len > PROTO_MAX_PAYLOAD) {
    return;
  }

  uint8_t buf[6U + PROTO_MAX_PAYLOAD + 2U];

  buf[0] = PROTO_HDR0;
  buf[1] = PROTO_HDR1;
  buf[2] = PROTO_VERSION;
  buf[3] = type;
  buf[4] = len;
  buf[5] = g_tx_seq++;

  for (uint8_t i = 0; i < len; i++) {
    buf[6U + i] = payload[i];
  }

  uint16_t crc =
      Proto_Crc16(&buf[2], (uint16_t)(4U + len));

  buf[6U + len] = (uint8_t)(crc & 0xFFU);
  buf[7U + len] = (uint8_t)((crc >> 8U) & 0xFFU);

  JetsonSerial.write(buf, (size_t)(8U + len));
}

static void Proto_SendStatus(const VehicleState *s) {
  uint8_t p[8];

  p[0] = (uint8_t)s->solMotor;
  p[1] = (uint8_t)s->sagMotor;
  p[2] = s->pan;
  p[3] = s->tilt;
  p[4] = s->lazer;
  p[5] = s->aktifMod;
  p[6] = s->elrsLink;
  p[7] = s->durum;

  Proto_SendFrame(TYPE_STATUS, p, 8U);
}

static void Proto_SendHeartbeat(uint32_t uptime_ms) {
  uint8_t p[5];

  p[0] = HB_KAYNAK_CONTROLLER;
  p[1] = (uint8_t)(uptime_ms & 0xFFU);
  p[2] = (uint8_t)((uptime_ms >> 8U) & 0xFFU);
  p[3] = (uint8_t)((uptime_ms >> 16U) & 0xFFU);
  p[4] = (uint8_t)((uptime_ms >> 24U) & 0xFFU);

  Proto_SendFrame(TYPE_HEARTBEAT, p, 5U);
}

static void Proto_OnValidFrame(uint32_t now_ms) {
  const uint8_t type = g_jrx_asm[1];
  const uint8_t len  = g_jrx_len;
  const uint8_t seq  = g_jrx_asm[3];
  const uint8_t *pl  = &g_jrx_asm[4];

  if (g_jrx_asm[0] != PROTO_VERSION) return;
  if (type == TYPE_COMMAND) {
    if (len != 7 || (int8_t)pl[0] < -100 || (int8_t)pl[0] > 100 ||
        (int8_t)pl[1] < -100 || (int8_t)pl[1] > 100 || pl[2] > 180 ||
        pl[3] > 180 || pl[4] > 1 || (pl[6] & ~CMD_FLAG_AUTO_REQ)) return;
  } else if (type == TYPE_HEARTBEAT) {
    if (len != 5 || pl[0] != HB_KAYNAK_JETSON) return;
  } else return;
  if (g_jetson_seen && (uint32_t)(now_ms-g_jetson_last_ms) >= JETSON_HB_TIMEOUT_MS)
    g_jrx_seq_valid = false;
  if (g_jrx_seq_valid) {
    uint8_t delta = (uint8_t)(seq - g_jrx_last_seq);
    if (delta == 0 || delta >= 128) return;
    g_rx_lost += delta - 1;
  }
  g_jrx_last_seq = seq;
  g_jrx_seq_valid = true;

  if ((type == TYPE_COMMAND) && (len >= 7U)) {
    int16_t sol = (int8_t)pl[0];
    int16_t sag = (int8_t)pl[1];

    if (sol < -100) sol = -100;
    if (sol >  100) sol =  100;
    if (sag < -100) sag = -100;
    if (sag >  100) sag =  100;

    g_cmd.solHedef   = (int8_t)sol;
    g_cmd.sagHedef   = (int8_t)sag;
    g_cmd.panHedef   = (pl[2] > 180U) ? 180U : pl[2];
    g_cmd.tiltHedef  = (pl[3] > 180U) ? 180U : pl[3];
    g_cmd.lazerKomut = (pl[4] != 0U) ? 1U : 0U;

    if ((pl[5] == 0U) ||
        (pl[5] == 1U) ||
        (pl[5] == MOD_KOMUT_DEGISTIRME)) {
      g_cmd.modKomut = pl[5];
    } else {
      g_cmd.modKomut = MOD_KOMUT_DEGISTIRME;
    }

    g_cmd.bayrak  = pl[6];
    g_cmd.sonAlim = now_ms;

    g_cmd_received   = true;
    g_jetson_seen    = true;
    g_jetson_last_ms = now_ms;
  }
  else if ((type == TYPE_HEARTBEAT) && (len >= 1U)) {
    if (pl[0] == HB_KAYNAK_JETSON) {
      g_jetson_seen    = true;
      g_jetson_last_ms = now_ms;
    }
  }
}

static void Proto_ParseByte(uint8_t b, uint32_t now_ms) {
  static uint32_t last_byte_ms = 0;
  if ((uint32_t)(now_ms-last_byte_ms) > 20) g_jrx_state = JRX_H0;
  last_byte_ms = now_ms;
  switch (g_jrx_state) {
    case JRX_H0:
      if (b == PROTO_HDR0) {
        g_jrx_state = JRX_H1;
      }
      break;

    case JRX_H1:
      if (b == PROTO_HDR1) {
        g_jrx_state = JRX_VER;
      }
      else if (b == PROTO_HDR0) {
        g_jrx_state = JRX_H1;
      }
      else {
        g_jrx_state = JRX_H0;
      }
      break;

    case JRX_VER:
      g_jrx_asm[0] = b;
      g_jrx_state = JRX_TYP;
      break;

    case JRX_TYP:
      g_jrx_asm[1] = b;
      g_jrx_state = JRX_LEN;
      break;

    case JRX_LEN:
      if (b > PROTO_MAX_PAYLOAD) {
        g_jrx_state = JRX_H0;
        break;
      }

      g_jrx_asm[2] = b;
      g_jrx_len = b;
      g_jrx_state = JRX_SEQ;
      break;

    case JRX_SEQ:
      g_jrx_asm[3] = b;
      g_jrx_i = 0U;
      g_jrx_state = (g_jrx_len > 0U) ? JRX_PAY : JRX_CRCL;
      break;

    case JRX_PAY:
      g_jrx_asm[4U + g_jrx_i] = b;
      g_jrx_i++;

      if (g_jrx_i >= g_jrx_len) {
        g_jrx_state = JRX_CRCL;
      }
      break;

    case JRX_CRCL:
      g_jrx_crc_lo = b;
      g_jrx_state = JRX_CRCH;
      break;

    case JRX_CRCH: {
      uint16_t crc_rx =
          (uint16_t)g_jrx_crc_lo |
          ((uint16_t)b << 8U);

      uint16_t crc_calc =
          Proto_Crc16(g_jrx_asm,
                      (uint16_t)(4U + g_jrx_len));

      if (crc_calc == crc_rx) {
        Proto_OnValidFrame(now_ms);
      } else {
        g_rx_crc_error = true;
      }

      g_jrx_state = JRX_H0;
      break;
    }

    default:
      g_jrx_state = JRX_H0;
      break;
  }
}

static void Proto_Poll(uint32_t now_ms) {
  unsigned budget = 512;
  while (budget-- && JetsonSerial.available() > 0) {
    int v = JetsonSerial.read();
    if (v >= 0) {
      Proto_ParseByte((uint8_t)v, now_ms);
    }
  }
}

static bool Proto_CommandFresh(uint32_t now_ms) {
  return g_cmd_received &&
         ((uint32_t)(now_ms - g_cmd.sonAlim) < CMD_TIMEOUT_MS);
}

static bool Proto_JetsonLinkFresh(uint32_t now_ms) {
  return g_jetson_seen &&
         ((uint32_t)(now_ms - g_jetson_last_ms) <
          JETSON_HB_TIMEOUT_MS);
}

static bool Proto_GetAndClearCrcError(void) {
  bool v = g_rx_crc_error;
  g_rx_crc_error = false;
  return v;
}

/* ==========================================================================
 * BNO055 IMU (I2C, register seviyesi - kutuphanesiz)
 *
 * Sadece telemetri: pitch/yaw okunup Jetson'a yollanir. Motor/servo/lazer
 * kontrolune KARISMAZ ve g_hw_fault'u TETIKLEMEZ. IMU yoksa/bozuksa yalnizca
 * IMU cercevesi gonderilmez; surus normal calismaya devam eder.
 * ========================================================================== */
#if IMU_ENABLED
#define BNO055_ID                  0xA0U
#define BNO055_REG_CHIP_ID         0x00U
#define BNO055_REG_PAGE_ID         0x07U
#define BNO055_REG_EUL_HEADING_L   0x1AU   /* 0x1A..0x1F: heading, roll, pitch */
#define BNO055_REG_CALIB_STAT      0x35U
#define BNO055_REG_OPR_MODE        0x3DU
#define BNO055_REG_PWR_MODE        0x3EU
#define BNO055_REG_SYS_TRIGGER     0x3FU
#define BNO055_OPR_CONFIG          0x00U
#define BNO055_OPR_NDOF            0x0CU   /* 9-DOF fuzyon, mutlak heading */
#define BNO055_PWR_NORMAL          0x00U

static bool    g_imu_ok   = false;
static uint8_t g_imu_fail = 0;

static bool Bno_Write8(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(BNO055_I2C_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static bool Bno_ReadLen(uint8_t reg, uint8_t *buf, uint8_t len) {
  Wire.beginTransmission(BNO055_I2C_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;      /* repeated start */
  if (Wire.requestFrom((int)BNO055_I2C_ADDR, (int)len) != len) return false;
  for (uint8_t i = 0; i < len; i++) buf[i] = (uint8_t)Wire.read();
  return true;
}

static bool Bno_Init(void) {
  Wire.begin(IMU_SDA_PIN, IMU_SCL_PIN, IMU_I2C_HZ);
  Wire.setTimeOut(25);   /* olu hatta ana dongu takilmasin */

  uint8_t id = 0;
  /* Acilistan sonra BNO055 ~650ms ID vermeyebilir; birkac deneme yap. */
  for (uint8_t i = 0; i < 15U && id != BNO055_ID; i++) {
    if (!Bno_ReadLen(BNO055_REG_CHIP_ID, &id, 1)) id = 0;
    if (id != BNO055_ID) delay(50);
  }
  if (id != BNO055_ID) return false;

  if (!Bno_Write8(BNO055_REG_OPR_MODE, BNO055_OPR_CONFIG)) return false;
  delay(25);
  Bno_Write8(BNO055_REG_PAGE_ID, 0x00U);
  Bno_Write8(BNO055_REG_PWR_MODE, BNO055_PWR_NORMAL);
  delay(10);
#if BNO055_USE_EXT_CRYSTAL
  Bno_Write8(BNO055_REG_SYS_TRIGGER, 0x80U);   /* harici 32kHz kristal */
#else
  Bno_Write8(BNO055_REG_SYS_TRIGGER, 0x00U);
#endif
  delay(10);
  if (!Bno_Write8(BNO055_REG_OPR_MODE, BNO055_OPR_NDOF)) return false;
  delay(25);
  return true;
}

/* pitch: derece*100 int16, yaw: derece*100 uint16 (0..35999), cal: durum bayti */
static bool Bno_Read(int16_t *pitch_cdeg, uint16_t *yaw_cdeg, uint8_t *cal) {
  uint8_t e[6];
  if (!Bno_ReadLen(BNO055_REG_EUL_HEADING_L, e, 6)) return false;

  int16_t head16 = (int16_t)((uint16_t)e[0] | ((uint16_t)e[1] << 8));   /* yaw   */
  int16_t pit16  = (int16_t)((uint16_t)e[4] | ((uint16_t)e[5] << 8));   /* pitch */

  /* BNO055 birimi 1/16 derece. derece*100 = deger * 100/16 = deger * 25/4 */
  int32_t yaw = ((int32_t)head16 * 25) / 4;
  int32_t pit = ((int32_t)pit16  * 25) / 4;

  while (yaw < 0)      yaw += 36000;
  while (yaw >= 36000) yaw -= 36000;
  if (pit >  32767) pit =  32767;
  if (pit < -32768) pit = -32768;

  *yaw_cdeg   = (uint16_t)yaw;
  *pitch_cdeg = (int16_t)pit;

  uint8_t c = 0;
  Bno_ReadLen(BNO055_REG_CALIB_STAT, &c, 1);   /* okunamazsa cal=0 gider */
  *cal = c;
  return true;
}

/* IMU cercevesi: pitch(int16), yaw(uint16), cal(uint8) -> 5 byte payload. */
static void Proto_SendImu(int16_t pitch_cdeg, uint16_t yaw_cdeg, uint8_t cal) {
  uint8_t p[5];
  p[0] = (uint8_t)(pitch_cdeg & 0xFFU);
  p[1] = (uint8_t)(((uint16_t)pitch_cdeg >> 8) & 0xFFU);
  p[2] = (uint8_t)(yaw_cdeg & 0xFFU);
  p[3] = (uint8_t)((yaw_cdeg >> 8) & 0xFFU);
  p[4] = cal;
  Proto_SendFrame(TYPE_IMU, p, 5U);
}
#endif /* IMU_ENABLED */

/* ==========================================================================
 * MOD / KONTROL
 * ========================================================================== */


static VehicleMode g_mode = MODE_MANUAL_DRIVE;

static VehicleMode Mode_ReadFromRadio(void) {
  int16_t auto_norm =
      Crsf_GetNorm(&g_crsf, CH_AUTO, 0U);

  if (AUTO_INPUT_SIGN * auto_norm > AUTO_SELECT_NORM) {
    return MODE_AUTONOMOUS;
  }

  uint16_t ch5_us =
      Crsf_GetUs(&g_crsf, CH_MANUAL_SERVO);

  if (ch5_us > MODE_HIGH_US) {
    return MODE_MANUAL_DRIVE;
  }

  return MODE_MANUAL_TURRET;
}

static bool Auto_Ready(uint32_t now_ms) {
  if (!Proto_JetsonLinkFresh(now_ms)) {
    return false;
  }

  if (!Proto_CommandFresh(now_ms)) {
    return false;
  }

  if ((g_cmd.bayrak & CMD_FLAG_AUTO_REQ) == 0U) {
    return false;
  }

  return true;
}

/* ==========================================================================
 * MOTOR STARTUP / ARMING GUVENLIGI
 * ========================================================================== */
static uint32_t    g_boot_ms = 0U;
static uint32_t    g_arm_candidate_since = 0U;
static bool        g_motor_armed = false;
static VehicleMode g_arm_mode = MODE_MANUAL_TURRET;

static void Safety_ResetMotorArm(VehicleMode mode) {
  g_motor_armed = false;
  g_arm_candidate_since = 0U;
  g_arm_mode = mode;
}

static bool Safety_MotorArmReady(uint32_t now_ms,
                                 bool link_ok,
                                 VehicleMode mode) {
  if (!link_ok || g_hw_fault) {
    Safety_ResetMotorArm(mode);
    return false;
  }

  if ((uint32_t)(now_ms - g_boot_ms) < STARTUP_HOLD_MS) {
    Safety_ResetMotorArm(mode);
    return false;
  }

  if (mode == MODE_MANUAL_TURRET) {
    Safety_ResetMotorArm(mode);
    return false;
  }

  if (mode != g_arm_mode) {
    Safety_ResetMotorArm(mode);
  }

  if (mode == MODE_AUTONOMOUS) {
    if (!Auto_Ready(now_ms)) {
      Safety_ResetMotorArm(mode);
      return false;
    }

    /* AUTO'da ikinci arm/0-komut beklemesi yok.
       Taze Jetson AUTO_REQ geldiginde motor yetkisi hemen acilir. */
    g_motor_armed = true;
    g_arm_candidate_since = 0U;
    g_arm_mode = mode;
    return true;
  }

  if (g_motor_armed) {
    return true;
  }

  bool safe_candidate = false;

  if (mode == MODE_MANUAL_DRIVE) {
    int16_t thr =
        Crsf_GetNorm(&g_crsf, CH_THROTTLE, STICK_DEADBAND_US);
    int16_t str =
        Crsf_GetNorm(&g_crsf, CH_STEER, STICK_DEADBAND_US);

    if ((thr >= -MANUAL_ARM_CENTER_NORM) &&
        (thr <=  MANUAL_ARM_CENTER_NORM) &&
        (str >= -MANUAL_ARM_CENTER_NORM) &&
        (str <=  MANUAL_ARM_CENTER_NORM)) {
      safe_candidate = true;
    }
  }
  if (!safe_candidate) {
    g_arm_candidate_since = 0U;
    return false;
  }

  if (g_arm_candidate_since == 0U) {
    g_arm_candidate_since = now_ms;
    return false;
  }

  if ((uint32_t)(now_ms - g_arm_candidate_since) >= ARM_STABLE_MS) {
    g_motor_armed = true;
    return true;
  }

  return false;
}

/* ==========================================================================
 * CIKIS DURUMLARI
 * ========================================================================== */
static int16_t g_applied_left  = 0;
static int16_t g_applied_right = 0;
static uint8_t g_fire_on       = 0;
static ManualDriveProfile g_manual_profile;

static void Safe_MotorsStop(void) {
  g_manual_profile.reset();
  Reactor_StopAll();
  Reactor_Enable(false);
  g_applied_left  = 0;
  g_applied_right = 0;
}

static void Laser_PinWrite(bool on) {
#if LASER_ACTIVE_HIGH
  digitalWrite(LASER_CTRL_PIN, on ? HIGH : LOW);
#else
  digitalWrite(LASER_CTRL_PIN, on ? LOW : HIGH);
#endif
}

static void Safe_LaserOff(void) {
  g_fire_on = 0U;
  Laser_PinWrite(false);
}

/* ==========================================================================
 * SURUS
 * ========================================================================== */
static void Drive_Update(uint32_t now_ms, bool link_ok) {
  VehicleMode mode =
      link_ok ? Mode_ReadFromRadio() : MODE_MANUAL_TURRET;

  if (!Safety_MotorArmReady(now_ms, link_ok, mode)) {
    Safe_MotorsStop();
    return;
  }

  if (mode == MODE_AUTONOMOUS) {
    if (!Auto_Ready(now_ms)) {
      Safety_ResetMotorArm(mode);
      Safe_MotorsStop();
      return;
    }

    int16_t left  = (int16_t)g_cmd.solHedef * AUTO_MAX_PCT / 10;
    int16_t right = (int16_t)g_cmd.sagHedef * AUTO_MAX_PCT / 10;

    Reactor_Enable(true);
    Reactor_SetDrive(left, right);

    g_applied_left  = left;
    g_applied_right = right;
    return;
  }

  if (mode != MODE_MANUAL_DRIVE) {
    Safe_MotorsStop();
    return;
  }

  Reactor_Enable(true);

  uint16_t sp_us =
      Crsf_GetUs(&g_crsf, CH_SPEED_MODE);

  int32_t max_pct = DRIVE_MAX_PCT_LOW;

  if (sp_us > 1700U) {
    max_pct = DRIVE_MAX_PCT_HIGH;
  }
  else if (sp_us > 1300U) {
    max_pct = DRIVE_MAX_PCT_MID;
  }

  int32_t thr =
      Crsf_GetNorm(&g_crsf,
                   CH_THROTTLE,
                   STICK_DEADBAND_US);

  int32_t str =
      Crsf_GetNorm(&g_crsf,
                   CH_STEER,
                   STICK_DEADBAND_US);

  /* Titreme-fix oncesindeki klasik manuel surus miksi.
     Donuste iki tarafin hizi zorla esitlenmez; gaz ve direksiyon toplanip/cikarilir. */
  thr *= DRIVE_THROTTLE_SIGN;
  str *= DRIVE_STEER_SIGN;
  str = (str * TURN_GAIN_PCT) / 100L;

  int32_t left  = thr + str;
  int32_t right = thr - str;

  int32_t peak = (left > 0) ? left : -left;
  int32_t tmp  = (right > 0) ? right : -right;

  if (tmp > peak) {
    peak = tmp;
  }

  if (peak > 1000) {
    left  = (left  * 1000L) / peak;
    right = (right * 1000L) / peak;
  }

  left  = (left  * max_pct) / 100L;
  right = (right * max_pct) / 100L;

  Reactor_SetDrive((int16_t)left, (int16_t)right);
  g_applied_left  = (int16_t)left;
  g_applied_right = (int16_t)right;
}

/* ==========================================================================
 * TARET
 * ========================================================================== */
static bool g_turret_armed = false;
static uint32_t g_turret_center_since = 0;
static bool g_laser_released = false;
static void Turret_ArmUpdate(uint32_t now_ms, bool link_ok) {
  if (!link_ok || g_hw_fault || g_mode != MODE_MANUAL_TURRET ||
      (uint32_t)(now_ms-g_boot_ms) < STARTUP_HOLD_MS) {
    g_turret_armed = false; g_turret_center_since=0; return;
  }
  if (g_turret_armed) return;
  if (Crsf_GetNorm(&g_crsf,CH_STEER,TURRET_DEADBAND_US) != 0 ||
      Crsf_GetNorm(&g_crsf,CH_THROTTLE,TURRET_DEADBAND_US) != 0) {
    g_turret_center_since=0; return;
  }
  if (!g_turret_center_since) g_turret_center_since=now_ms;
  if ((uint32_t)(now_ms-g_turret_center_since)>=ARM_STABLE_MS) {
    g_turret_armed=true;
    ServoPan_WriteUs(g_pan_us); ServoTilt_WriteUs(g_tilt_us);
  }
}

static int32_t Servo_ApproachTarget(int32_t current,int32_t target,int32_t rate,uint32_t dt) {
  int32_t error=target-current;
  if(motionAbs(error)<=SERVO_AUTO_HOLD_US)return current;
  int32_t step=rate*(int32_t)dt/1000;
  if(step<1)step=1;
  if(error>step)error=step; if(error< -step)error=-step;
  return current+error;
}

static void Turret_Update(uint32_t now_ms, bool link_ok) {
  if (!link_ok || g_hw_fault) { Servo_ResetInputFilters(); return; }
  VehicleMode mode=Mode_ReadFromRadio();
  if ((mode==MODE_AUTONOMOUS && (!Auto_Ready(now_ms) || !g_motor_armed)) ||
      (mode==MODE_MANUAL_TURRET && !g_turret_armed) || mode==MODE_MANUAL_DRIVE) {
    Servo_ResetInputFilters();return;
  }
  uint32_t dt=(uint32_t)(now_ms-g_servo_update_ms);
  if(dt<SERVO_UPDATE_MS)return;
  g_servo_update_ms=now_ms;
  if(dt>40)dt=40;
  if(mode==MODE_AUTONOMOUS){
    int32_t pan=Servo_DegToUsCentered(PAN_MIN_US,PAN_CENTER_US,PAN_MAX_US,g_cmd.panHedef);
    int32_t tilt=Servo_DegToUsCentered(TILT_MIN_US,TILT_CENTER_US,TILT_MAX_US,g_cmd.tiltHedef);
    ServoPan_WriteUs(Servo_ApproachTarget(g_pan_us,pan,PAN_RATE_US_PER_S,dt));
    ServoTilt_WriteUs(Servo_ApproachTarget(g_tilt_us,tilt,TILT_RATE_US_PER_S,dt));
    return;
  }
  int32_t pan_in=g_pan_filter.update((int32_t)Crsf_GetUs(&g_crsf,CH_STEER)-1500,
                     SERVO_INPUT_ENTER_US,SERVO_INPUT_EXIT_US)*PAN_INPUT_SIGN;
  int32_t tilt_in=g_tilt_filter.update((int32_t)Crsf_GetUs(&g_crsf,CH_THROTTLE)-1500,
                     SERVO_INPUT_ENTER_US,SERVO_INPUT_EXIT_US)*TILT_INPUT_SIGN;
  if(pan_in==0)g_pan_rate_accum=0;
  if(tilt_in==0)g_tilt_rate_accum=0;
  g_pan_rate_accum+=PAN_RATE_US_PER_S*pan_in*(int32_t)dt;
  g_tilt_rate_accum+=TILT_RATE_US_PER_S*tilt_in*(int32_t)dt;
  const int32_t quantum=1000000L*SERVO_POSITION_STEP_US;
  int32_t d_pan=(g_pan_rate_accum/quantum)*SERVO_POSITION_STEP_US;
  int32_t d_tilt=(g_tilt_rate_accum/quantum)*SERVO_POSITION_STEP_US;
  g_pan_rate_accum-=d_pan*1000000L;
  g_tilt_rate_accum-=d_tilt*1000000L;
  if(d_pan)ServoPan_AddUs(d_pan);
  if(d_tilt)ServoTilt_AddUs(d_tilt);
}

/* ==========================================================================
 * LAZER
 * ========================================================================== */
static void Laser_Update(uint32_t now_ms, bool link_ok) {
  if (!link_ok || g_hw_fault) {
    Safe_LaserOff();
    return;
  }

  VehicleMode mode = Mode_ReadFromRadio();

  if (mode == MODE_MANUAL_DRIVE) {
    Safe_LaserOff();
    return;
  }

  if (mode == MODE_MANUAL_TURRET) {
    int16_t fire_norm =
        Crsf_GetNorm(&g_crsf, CH_FIRE, 0U);

    if (fire_norm <= 500) g_laser_released = true;
    g_fire_on = (g_turret_armed && g_laser_released && fire_norm > 500) ? 1U : 0U;

    Laser_PinWrite(g_fire_on != 0U);

    return;
  }

  /* OTONOM */
  if (!Auto_Ready(now_ms) || !g_motor_armed) {
    Safe_LaserOff();
    return;
  }

  g_fire_on =
      (g_cmd.lazerKomut != 0U) ? 1U : 0U;

  Laser_PinWrite(g_fire_on != 0U);
}

/* ==========================================================================
 * TELEMETRI
 * ========================================================================== */
static void Telemetry_Update(uint32_t now_ms, bool link_ok) {
  static uint32_t t_status = 0;
  static uint32_t t_hb = 0;
#if IMU_ENABLED
  static uint32_t t_imu = 0;
#endif

  if ((uint32_t)(now_ms - t_status) >= STATUS_PERIOD_MS) {
    t_status = now_ms;

    VehicleState st;
    VehicleMode mode =
        link_ok ? Mode_ReadFromRadio() : MODE_MANUAL_DRIVE;

    st.solMotor = (int8_t)(g_applied_left / 10);
    st.sagMotor = (int8_t)(g_applied_right / 10);

    st.pan =
        Servo_UsToDegCentered(
            g_pan_us,
            PAN_MIN_US,
            PAN_CENTER_US,
            PAN_MAX_US);

    st.tilt =
        Servo_UsToDegCentered(
            g_tilt_us,
            TILT_MIN_US,
            TILT_CENTER_US,
            TILT_MAX_US);

    st.lazer    = g_fire_on;
    st.aktifMod = (uint8_t)mode;
    st.elrsLink = link_ok ? 1U : 0U;

    uint8_t durum = 0U;

    if (!link_ok) {
      durum |= ST_FAILSAFE;
    }

    if (Proto_JetsonLinkFresh(now_ms)) {
      durum |= ST_JETSON_LINK;
    }

    if ((mode == MODE_AUTONOMOUS) && link_ok) {
      if (!Proto_CommandFresh(now_ms)) {
        durum |= ST_CMD_TIMEOUT;
      }

      if (Auto_Ready(now_ms)) {
        durum |= ST_AUTO_EN;
      } else {
        durum |= ST_FAILSAFE;
      }
    }

    if (Proto_GetAndClearCrcError()) {
      durum |= ST_CRC_ERR;
    }

    if (g_motor_armed) durum |= ST_ARMED;
    if (g_hw_fault) durum |= ST_HW_ERROR | ST_FAILSAFE;
    st.durum = durum;
    Proto_SendStatus(&st);
  }

  if ((uint32_t)(now_ms - t_hb) >= HB_PERIOD_MS) {
    t_hb = now_ms;
    Proto_SendHeartbeat(now_ms);
  }

#if IMU_ENABLED
  if (g_imu_ok && (uint32_t)(now_ms - t_imu) >= IMU_PERIOD_MS) {
    t_imu = now_ms;
    int16_t  pit;
    uint16_t yaw;
    uint8_t  cal;
    if (Bno_Read(&pit, &yaw, &cal)) {
      g_imu_fail = 0;
      Proto_SendImu(pit, yaw, cal);
    } else if (++g_imu_fail >= 20U) {
      g_imu_ok = false;   /* ~1s okunamadi -> vazgec; surus/motor ETKILENMEZ */
    }
  }
#endif
}

/* ==========================================================================
 * DEBUG
 * ========================================================================== */
static const char *Mode_Name(VehicleMode mode) {
  switch (mode) {
    case MODE_MANUAL_DRIVE:  return "MANUAL";
    case MODE_MANUAL_TURRET: return "SERVO_LASER";
    case MODE_AUTONOMOUS:    return "AUTO";
    default:                 return "?";
  }
}

static void Debug_Update(uint32_t now_ms, bool link_ok) {
#if DEBUG_ENABLED
  static uint32_t last_debug = 0;

  if ((uint32_t)(now_ms - last_debug) < DEBUG_PERIOD_MS) {
    return;
  }

  last_debug = now_ms;
  if (Serial.availableForWrite() < 384) return;

  VehicleMode mode =
      link_ok ? Mode_ReadFromRadio() : MODE_MANUAL_DRIVE;

  Serial.printf(
      "LINK:%s MODE:%s "
      "CH1:%u CH2:%u CH5:%u CH6:%u CH8:%u CH10:%u "
      "PAN:%ld TILT:%ld L:%d R:%d LASER:%u "
      "ARM:%u STARTUP:%u "
      "JETSON:%u CMD:%u AUTO_READY:%u LOST:%u CRC:%lu\n",
      link_ok ? "UP" : "DOWN",
      Mode_Name(mode),
      (unsigned)Crsf_GetUs(&g_crsf, CH_STEER),
      (unsigned)Crsf_GetUs(&g_crsf, CH_THROTTLE),
      (unsigned)Crsf_GetUs(&g_crsf, CH_MANUAL_SERVO),
      (unsigned)Crsf_GetUs(&g_crsf, CH_SPEED_MODE),
      (unsigned)Crsf_GetUs(&g_crsf, CH_AUTO),
      (unsigned)Crsf_GetUs(&g_crsf, CH_FIRE),
      (long)g_pan_us,
      (long)g_tilt_us,
      (int)g_applied_left,
      (int)g_applied_right,
      (unsigned)g_fire_on,
      (unsigned)g_motor_armed,
      (unsigned)(((uint32_t)(now_ms - g_boot_ms) >= STARTUP_HOLD_MS) ? 1U : 0U),
      (unsigned)Proto_JetsonLinkFresh(now_ms),
      (unsigned)Proto_CommandFresh(now_ms),
      (unsigned)Auto_Ready(now_ms),
      (unsigned)g_rx_lost,
      (unsigned long)g_crsf.crc_error_count);
#else
  (void)now_ms;
  (void)link_ok;
#endif
}

/* ==========================================================================
 * ARDUINO SETUP / LOOP
 * ========================================================================== */
static uint32_t g_last_loop_ms = 0;

void ikaSetup() {
  /* Motor/lazer guvenligi Serial.begin ve tum delay'lerden ONCE. */
  if (REACTOR_EN_PIN >= 0) { Reactor_Enable(false); pinMode(REACTOR_EN_PIN, OUTPUT); }
  pinMode(REACTOR_LEFT_TX_PIN, OUTPUT);
  pinMode(REACTOR_RIGHT_TX_PIN, OUTPUT);
  Laser_PinWrite(false);
  pinMode(LASER_CTRL_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);

  Reactor_Enable(false);
  digitalWrite(REACTOR_LEFT_TX_PIN, HIGH);
  digitalWrite(REACTOR_RIGHT_TX_PIN, HIGH);
  Laser_PinWrite(false);
  digitalWrite(LED_PIN, LOW);

  g_boot_ms = millis();
  Safety_ResetMotorArm(MODE_MANUAL_TURRET);

  Reactor_Init();
  for (uint8_t i = 0; i < 5U; i++) {
    Reactor_StopAll();
    delay(10);
  }

  Serial.setTxBufferSize(1024);
  Serial.begin(DEBUG_BAUD);
  delay(50);

  Serial.println();
  Serial.println("======================================================");
  Serial.println(" IKA ESP32 V2 - YUMUSAK PIVOT / SERVO FILTRE");
  Serial.println("======================================================");
  Serial.println(" ELRS RX       : GPIO16 @420000");
  Serial.println(" Left Reactor  : GPIO25 @38400 RMT-TX");
  Serial.println(" Right Reactor : GPIO26 @38400 RMT-TX");
  Serial.println(" PAN servo     : GPIO18");
  Serial.println(" TILT servo    : GPIO19");
  Serial.println(" Laser         : GPIO27");
  Serial.println(" Reactor EN    : config.h REACTOR_EN_PIN (-1 default)");
  Serial.println(" Jetson RX     : GPIO21 <- USB-TTL TX");
  Serial.println(" Jetson TX     : GPIO22 -> USB-TTL RX");
#if IMU_ENABLED
  Serial.println(" IMU BNO055    : SDA=GPIO32 SCL=GPIO33 (sadece telemetri)");
#endif
  Serial.println(" CH8 high      : AUTO");
  Serial.println(" CH5 high      : MANUAL");
  Serial.println(" CH5 low       : SERVO+LASER");
  Serial.println(" CH6           : SPEED 30/60/100; PIVOT MAX 35%");
  Serial.println(" V2            : EQUAL DRIVE / ZERO CROSS HOLD / SERVO FILTER");
  Serial.println(" CH10 high     : LASER");
  Serial.println(" Startup guard : 1.2s + 0.5s safe arm");
  Serial.println("======================================================");

  ServoPwm_Init();
  // No servo pulse at boot. First valid turret/auto session enables hold.
  ledcWrite(SERVO_PAN_PIN, 0);
  ledcWrite(SERVO_TILT_PIN, 0);

  Serial2.setRxBufferSize(512);
  Serial2.begin(
      CRSF_BAUD,
      SERIAL_8N1,
      CRSF_RX_PIN,
      CRSF_TX_PIN);

  JetsonSerial.setRxBufferSize(512);
  JetsonSerial.begin(
      JETSON_BAUD,
      SERIAL_8N1,
      JETSON_RX_PIN,
      JETSON_TX_PIN);

  Crsf_Init(&g_crsf);

  g_cmd.solHedef   = 0;
  g_cmd.sagHedef   = 0;
  g_cmd.panHedef   = 90U;
  g_cmd.tiltHedef  = 90U;
  g_cmd.lazerKomut = 0U;
  g_cmd.modKomut   = MOD_KOMUT_DEGISTIRME;
  g_cmd.bayrak     = 0U;
  g_cmd.sonAlim    = 0U;

  Reactor_StopAll();
  Reactor_Enable(false);
  Safe_LaserOff();

#if IMU_ENABLED
  /* IMU en son baslatilir; basarisiz olsa bile motor/servo guvenligini etkilemez. */
  g_imu_ok   = Bno_Init();
  g_imu_fail = 0;
  Serial.println(g_imu_ok
      ? " IMU BNO055    : OK (NDOF) - pitch/yaw telemetri aktif"
      : " IMU BNO055    : YOK/HATA - surus etkilenmez, IMU gonderilmez");
#endif

  g_last_loop_ms = millis();

  Serial.println("Sistem hazir. Ilk testte tekerlekler havada olsun.");
}


void ikaLoop() {
  uint32_t now = millis();
  Crsf_Poll(now);
  bool link_ok = Crsf_IsLinkUp(&g_crsf, now);
  VehicleMode next = link_ok ? Mode_ReadFromRadio() : MODE_MANUAL_DRIVE;
  static bool previous_link = false;
  if (next != g_mode || previous_link != link_ok) {
    Safe_LaserOff(); Safe_MotorsStop();
    Safety_ResetMotorArm(next);
    g_turret_armed = false; g_turret_center_since = 0; g_laser_released=false;
    Servo_ResetInputFilters();
    g_cmd_received=false; g_jrx_state=JRX_H0;
    // Discard commands that were queued before this mode transition.
    unsigned budget=512;
    while (budget-- && JetsonSerial.available()) JetsonSerial.read();
    g_mode=next; previous_link=link_ok;
  }
  Proto_Poll(now);
  if ((uint32_t)(now-g_last_loop_ms) < LOOP_PERIOD_MS) return;
  g_last_loop_ms=now;
  Drive_Update(now,link_ok);
  Turret_ArmUpdate(now,link_ok);
  Turret_Update(now,link_ok);
  Laser_Update(now,link_ok);
  if (g_hw_fault) { Safe_LaserOff(); Safe_MotorsStop(); Safety_ResetMotorArm(g_mode); }
  digitalWrite(LED_PIN, link_ok && !g_hw_fault ? HIGH : LOW);
  Telemetry_Update(now,link_ok);
  Debug_Update(now,link_ok);
}
