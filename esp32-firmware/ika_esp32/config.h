#pragma once
// Kullanici ayarlari. Pinler ESP32-WROOM-32 icindir.
#define CRSF_RX_PIN               16
#define CRSF_TX_PIN               -1
#define REACTOR_LEFT_TX_PIN       25
#define REACTOR_RIGHT_TX_PIN      26
#define REACTOR_EN_PIN            (-1)
#define SERVO_PAN_PIN             18
#define SERVO_TILT_PIN            19
#define LASER_CTRL_PIN            27
#define JETSON_RX_PIN             21   /* USB-TTL TX -> ESP32 RX */
#define JETSON_TX_PIN             22   /* USB-TTL RX <- ESP32 TX */
#define LED_PIN                    2
#define REACTOR_EN_ACTIVE_HIGH          1
#define LASER_ACTIVE_HIGH               1
#define DRIVE_MAX_PCT_LOW           30
#define DRIVE_MAX_PCT_MID           60
#define DRIVE_MAX_PCT_HIGH         100
#define TURN_GAIN_PCT               60
#define INV_LEFT_A                 (+1)
#define INV_LEFT_B                 (+1)
#define INV_RIGHT_A                (-1)
#define INV_RIGHT_B                (-1)
#define PAN_MIN_US                  700
#define PAN_MAX_US                 2300
#define PAN_CENTER_US              1500
#define TILT_UP_EXTRA_US              50
#define TILT_MIN_US (1100 - ((TILT_INPUT_SIGN < 0) ? TILT_UP_EXTRA_US : 0))
#define TILT_MAX_US (1900 + ((TILT_INPUT_SIGN > 0) ? TILT_UP_EXTRA_US : 0))
#define TILT_CENTER_US            1500
#define PAN_INPUT_SIGN              (-1)
#define TILT_INPUT_SIGN             (-1)
#define PAN_RATE_US_PER_S            400
#define TILT_RATE_US_PER_S           250

// C en altta +100 (yaklasik 2000us). Ters cikiyorsa -1 yapin.
#define AUTO_INPUT_SIGN (+1)
#define DRIVE_THROTTLE_SIGN (+1)
#define DRIVE_STEER_SIGN (+1)
// Otonom hiz tavanidir; ilk yer testi icin dusurun.
#define AUTO_MAX_PCT 100
// EN -1: ek enable kablosu yok; sadece UART stop komutu.
// EN kullanilacaksa 5V uyumlu tampon ve bypass ayari gerekir; README'yi okuyun.
// Role karti LOW ile cekiyorsa LASER_ACTIVE_HIGH 0 yapin.
// Servo limitleri mekaniginize gore AYARLANMALIDIR; derece olcumu degildir.

// V2: manuel suruste donus oncelikli yerinde donus; hareket boyunca |L|=|R|.
#define MANUAL_TURN_MAX_PCT           35
#define MANUAL_PIVOT_ENTER_NORM      180
#define MANUAL_PIVOT_EXIT_NORM       120
#define MANUAL_ACCEL_PCT_PER_S        70
#define MANUAL_DECEL_PCT_PER_S       150
#define MANUAL_DIRECTION_HOLD_MS     150
// V2: servo giris filtresi; PWM hala donanim LEDC ile 50 Hz.
#define SERVO_UPDATE_MS               20
#define SERVO_INPUT_ENTER_US          70
#define SERVO_INPUT_EXIT_US           45
#define SERVO_POSITION_STEP_US         2
#define SERVO_AUTO_HOLD_US             3

// IMU: BNO055 (I2C). Jetson'a pitch/yaw telemetrisi gonderir; motor/servo/lazer
// kontroluna KARISMAZ. IMU yoksa/bozuksa sadece IMU cercevesi gitmez, surus etkilenmez.
// Varsayilan I2C pinleri (21/22) Jetson UART1'e ayrildigi icin bos GPIO32/33 kullanilir.
#ifndef IMU_ENABLED
#define IMU_ENABLED                    1   /* host testleri -DIMU_ENABLED=0 ile kapatir */
#endif
#define IMU_SDA_PIN                   32
#define IMU_SCL_PIN                   33
#define IMU_I2C_HZ               100000UL   /* BNO055 clock-stretch icin 100kHz */
#define BNO055_I2C_ADDR             0x28    /* ADR pini bos -> 0x28 */
// Adafruit BNO055 breakout'unda harici 32.768kHz kristal vardir -> 1.
// Ciplak cip / kristalsiz modulde 0 yapin.
#define BNO055_USE_EXT_CRYSTAL         1

static_assert(SERVO_INPUT_EXIT_US > 0 && SERVO_INPUT_ENTER_US > SERVO_INPUT_EXIT_US && SERVO_INPUT_ENTER_US < 512, "Servo deadband ayarlari gecersiz");
static_assert(MANUAL_PIVOT_ENTER_NORM > MANUAL_PIVOT_EXIT_NORM && MANUAL_PIVOT_EXIT_NORM >= 0, "Pivot esikleri gecersiz");
static_assert(TILT_MIN_US > 0 && TILT_MIN_US < TILT_CENTER_US && TILT_CENTER_US < TILT_MAX_US, "Tilt limitleri gecersiz");
static_assert(MANUAL_ACCEL_PCT_PER_S > 0 && MANUAL_DECEL_PCT_PER_S > 0, "Rampa hizi sifirdan buyuk olmali");
