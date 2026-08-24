#include <Arduino.h>
#include <HardwareSerial.h>
#include <AlfredoCRSF.h>
#include <HardwareTimer.h>   // Pan/Tilt servolar icin donanim PWM
#include "haberlesme.h"      // [HABERLESME] modul

// ======================================================
// PROFESYONEL TANK DRIVE - CRSF KUMANDA
// ======================================================

// ------------------------------------------------------
// TANK MOTOR PINLERI
// ------------------------------------------------------
#define SOL_ON_RPWM    PA0
#define SOL_ON_LPWM    PA1
#define SOL_ARKA_RPWM  PB8
#define SOL_ARKA_LPWM  PB9
#define SAG_DIR        PA2
#define SAG_PWM        PA3

#define MOTOR_PWM_HZ   20000  // 20 kHz
#define MAX_GUC        50     // Maksimum guc (%)
#define DEADZONE       5      // Kumanda olu bolgesi (%)

// CRSF: PA10 = RX, PA9 = TX
HardwareSerial crsfSerial(PA10, PA9);
AlfredoCRSF crsf;

// ======================================================
// LAZER PAN/TILT SERVO + ATESLEME PINLERI
// ======================================================
#define SERVO1_PIN   PB0   // Pan  (yatay) - TIM3_CH3
#define SERVO2_PIN   PB1   // Tilt (dikey) - TIM3_CH4
#define ATES_PIN     PA4   // Lazer atesleme cikisi

#define SERVO_PERIOD_US   20000   // 20ms = 50Hz
#define SERVO_MIN_US      500
#define SERVO_MAX_US      2500

#define SERVO_MIN_ACI     30
#define SERVO_MAX_ACI     150
#define SERVO_BASLANGIC   90
#define SERVO_HIZ         1.2
#define SERVO_DEADZONE    10

#define SERVO_MODA_GIRINCE_ORTALA  0

HardwareTimer *srv1Tim = NULL;
HardwareTimer *srv2Tim = NULL;
uint32_t srv1Ch;
uint32_t srv2Ch;

float servo1Aci = SERVO_BASLANGIC;   // Pan
float servo2Aci = SERVO_BASLANGIC;   // Tilt

// ======================================================
// MOD SECIMI
// ======================================================
#define MOD_SURUS        0
#define MOD_LAZER        1

#define MOD_SURUS_ESIK   1200
#define MOD_LAZER_ESIK   1800
#define MOD_ONAY_SAYISI  7

#define ATES_ESIK        1500

int aktifMod  = MOD_SURUS;
int oncekiMod = MOD_SURUS;
int modSayac  = 0;

// ======================================================
// [HABERLESME] OTONOM ARBITRASYON (Secenek A)
// ======================================================
#define AUTO_KANAL       6      // CRSF: operator AUTO anahtari
#define AUTO_ESIK        1500   // > esik ise AUTO izni

VehicleState arac;              // [HABERLESME] telemetri kaynagi

// [HABERLESME] uygulanan motor komutlari (MAX_GUC sonrasi, telemetri icin)
int  g_solUyg = 0;
int  g_sagUyg = 0;
bool g_lazerAcik = false;       // atesleme durumu
bool g_failsafe  = false;       // guvenlik durdurmasi aktif mi

// ======================================================
// YARDIMCI FONKSIYONLAR
// ======================================================
int yuzdeCevir(int ham) {
  if (ham < 900 || ham > 2100) return 0;
  int deger = map(ham, 1000, 2000, -100, 100);
  deger = constrain(deger, -100, 100);
  if (abs(deger) < DEADZONE) deger = 0;
  return deger;
}

void motorSur_2PWM(int rpwm, int lpwm, int komut) {
  komut = constrain(komut, -100, 100);
  int pwm = map(abs(komut), 0, 100, 0, 255);
  if (komut > 0) {
    analogWrite(lpwm, 0);
    analogWrite(rpwm, pwm);
  } else if (komut < 0) {
    analogWrite(rpwm, 0);
    analogWrite(lpwm, pwm);
  } else {
    analogWrite(rpwm, 0);
    analogWrite(lpwm, 0);
  }
}

void motorSur_DIR_PWM(int dir_pin, int pwm_pin, int komut) {
  komut = constrain(komut, -100, 100);
  int pwm = map(abs(komut), 0, 100, 0, 255);
  if (komut > 0) {
    digitalWrite(dir_pin, HIGH);
    analogWrite(pwm_pin, pwm);
  } else if (komut < 0) {
    digitalWrite(dir_pin, LOW);
    analogWrite(pwm_pin, pwm);
  } else {
    analogWrite(pwm_pin, 0);
  }
}

void hepsiniDurdur() {
  analogWrite(SOL_ON_RPWM, 0);
  analogWrite(SOL_ON_LPWM, 0);
  analogWrite(SOL_ARKA_RPWM, 0);
  analogWrite(SOL_ARKA_LPWM, 0);
  analogWrite(SAG_PWM, 0);
}

// ======================================================
// SERVO DONANIM PWM FONKSIYONLARI
// ======================================================
int aciToUs(float aci) {
  aci = constrain(aci, 0, 180);
  return (int)(SERVO_MIN_US + (aci / 180.0) * (SERVO_MAX_US - SERVO_MIN_US));
}

void servoBaslat() {
  PinName p1 = digitalPinToPinName(SERVO1_PIN);
  PinName p2 = digitalPinToPinName(SERVO2_PIN);

  TIM_TypeDef *inst1 = (TIM_TypeDef *)pinmap_peripheral(p1, PinMap_PWM);
  TIM_TypeDef *inst2 = (TIM_TypeDef *)pinmap_peripheral(p2, PinMap_PWM);

  srv1Ch = STM_PIN_CHANNEL(pinmap_function(p1, PinMap_PWM));
  srv2Ch = STM_PIN_CHANNEL(pinmap_function(p2, PinMap_PWM));

  srv1Tim = new HardwareTimer(inst1);
  srv1Tim->setMode(srv1Ch, TIMER_OUTPUT_COMPARE_PWM1, SERVO1_PIN);
  srv1Tim->setOverflow(SERVO_PERIOD_US, MICROSEC_FORMAT);
  srv1Tim->setCaptureCompare(srv1Ch, aciToUs(SERVO_BASLANGIC), MICROSEC_COMPARE_FORMAT);

  if (inst2 == inst1) {
    srv2Tim = srv1Tim;
    srv2Tim->setMode(srv2Ch, TIMER_OUTPUT_COMPARE_PWM1, SERVO2_PIN);
    srv2Tim->setCaptureCompare(srv2Ch, aciToUs(SERVO_BASLANGIC), MICROSEC_COMPARE_FORMAT);
  } else {
    srv2Tim = new HardwareTimer(inst2);
    srv2Tim->setMode(srv2Ch, TIMER_OUTPUT_COMPARE_PWM1, SERVO2_PIN);
    srv2Tim->setOverflow(SERVO_PERIOD_US, MICROSEC_FORMAT);
    srv2Tim->setCaptureCompare(srv2Ch, aciToUs(SERVO_BASLANGIC), MICROSEC_COMPARE_FORMAT);
    srv2Tim->resume();
  }
  srv1Tim->resume();
}

void servoYaz1(float aci) {
  srv1Tim->setCaptureCompare(srv1Ch, aciToUs(aci), MICROSEC_COMPARE_FORMAT);
}
void servoYaz2(float aci) {
  srv2Tim->setCaptureCompare(srv2Ch, aciToUs(aci), MICROSEC_COMPARE_FORMAT);
}

// ======================================================
// ATESLEME KONTROLU
// ======================================================
void ateslemeKapat() {
  digitalWrite(ATES_PIN, LOW);
  g_lazerAcik = false;          // [HABERLESME] durum takibi
}
void ateslemeAc() {
  digitalWrite(ATES_PIN, HIGH);
  g_lazerAcik = true;           // [HABERLESME] durum takibi
}

// ======================================================
// SURUS MODU (MANUEL - orijinal mantik)
// ======================================================
void surusModu() {
  int donus = yuzdeCevir(crsf.getChannel(1));
  int gaz   = yuzdeCevir(crsf.getChannel(2));

  int sol = 0;
  int sag = 0;

  if (gaz == 0) {
    sol = donus;
    sag = -donus;
  } else {
    if (donus >= 0) {
      sol = gaz;
      sag = map(donus, 0, 100, gaz, 0);
    } else {
      sol = map(abs(donus), 0, 100, gaz, 0);
      sag = gaz;
    }
  }

  sol = constrain(sol, -100, 100);
  sag = constrain(sag, -100, 100);

  sol = sol * MAX_GUC / 100;
  sag = sag * MAX_GUC / 100;

  motorSur_2PWM(SOL_ON_RPWM, SOL_ON_LPWM, sol);
  motorSur_2PWM(SOL_ARKA_RPWM, SOL_ARKA_LPWM, sol);
  motorSur_DIR_PWM(SAG_DIR, SAG_PWM, sag);

  g_solUyg = sol;   // [HABERLESME] telemetri
  g_sagUyg = sag;   // [HABERLESME]
}

// ======================================================
// LAZER MODU (MANUEL - orijinal mantik)
// ======================================================
void lazerModu() {
  hepsiniDurdur();

  int panGirdi  = yuzdeCevir(crsf.getChannel(1));
  int tiltGirdi = yuzdeCevir(crsf.getChannel(2));
  int chAtes    = crsf.getChannel(3);

  if (abs(panGirdi)  < SERVO_DEADZONE) panGirdi  = 0;
  if (abs(tiltGirdi) < SERVO_DEADZONE) tiltGirdi = 0;

  servo1Aci += (panGirdi  / 100.0) * SERVO_HIZ;
  servo2Aci += (tiltGirdi / 100.0) * SERVO_HIZ;

  servo1Aci = constrain(servo1Aci, SERVO_MIN_ACI, SERVO_MAX_ACI);
  servo2Aci = constrain(servo2Aci, SERVO_MIN_ACI, SERVO_MAX_ACI);

  servoYaz1(servo1Aci);
  servoYaz2(servo2Aci);

  bool ates = (chAtes > ATES_ESIK);
  if (ates) ateslemeAc();
  else      ateslemeKapat();

  g_solUyg = 0;   // [HABERLESME] motorlar duruyor
  g_sagUyg = 0;   // [HABERLESME]
}

// ======================================================
// [HABERLESME] OTONOM MODLAR (Jetson hedefleri, ayni suruculer)
// ======================================================
bool otonomIzinliMi() {
  bool anahtar = (crsf.getChannel(AUTO_KANAL) > AUTO_ESIK);   // operator izni
  bool istek   = (jetsonKomut.bayrak & CMD_FLAG_AUTO_REQ);    // Jetson istegi
  return anahtar && istek && komutTaze() && jetsonLinkTaze();
  // Not: CRSF link zaten loop basinda kontrol ediliyor; buraya gelindiyse UP.
}

void otonomSurus() {
  ateslemeKapat();
  int sol = constrain((int)jetsonKomut.solHedef, -100, 100);
  int sag = constrain((int)jetsonKomut.sagHedef, -100, 100);

  sol = sol * MAX_GUC / 100;
  sag = sag * MAX_GUC / 100;

  motorSur_2PWM(SOL_ON_RPWM, SOL_ON_LPWM, sol);
  motorSur_2PWM(SOL_ARKA_RPWM, SOL_ARKA_LPWM, sol);
  motorSur_DIR_PWM(SAG_DIR, SAG_PWM, sag);

  g_solUyg = sol;
  g_sagUyg = sag;
}

void otonomLazer() {
  hepsiniDurdur();

  // nihai (yetkili) servo clamp'i burada
  servo1Aci = constrain((float)jetsonKomut.panHedef,  SERVO_MIN_ACI, SERVO_MAX_ACI);
  servo2Aci = constrain((float)jetsonKomut.tiltHedef, SERVO_MIN_ACI, SERVO_MAX_ACI);
  servoYaz1(servo1Aci);
  servoYaz2(servo2Aci);

  // lazer yalnizca komut taze + acikca 1 iken; aksi OFF (guvenli varsayilan)
  if (jetsonKomut.lazerKomut && komutTaze()) ateslemeAc();
  else                                       ateslemeKapat();

  g_solUyg = 0;
  g_sagUyg = 0;
}

// ======================================================
// [HABERLESME] DURUM DOLDUR + PERIYODIK GONDER
// ======================================================
void durumGuncelle(bool linkUp, bool otonom) {
  arac.solMotor = (int8_t)constrain(g_solUyg, -100, 100);
  arac.sagMotor = (int8_t)constrain(g_sagUyg, -100, 100);
  arac.pan      = (uint8_t)constrain((int)servo1Aci, 0, 180);
  arac.tilt     = (uint8_t)constrain((int)servo2Aci, 0, 180);
  arac.lazer    = g_lazerAcik ? 1 : 0;
  arac.aktifMod = (uint8_t)aktifMod;
  arac.elrsLink = linkUp ? 1 : 0;

  uint8_t f = 0;
  if (jetsonLinkTaze())  f |= ST_JETSON_LINK;
  if (!komutTaze())      f |= ST_CMD_TIMEOUT;
  if (otonom)            f |= ST_AUTO_EN;
  if (g_failsafe)        f |= ST_FAILSAFE;
  if (haberlesmeCrcHata())f |= ST_CRC_ERR;
  arac.durum = f;
}

void haberlesmePeriyodik(bool linkUp, bool otonom) {
  static unsigned long tStatus = 0, tHb = 0;
  unsigned long now = millis();
  if (now - tStatus >= STATUS_PERIOD_MS) {
    tStatus = now;
    durumGuncelle(linkUp, otonom);
    telemetriGonder(arac);
  }
  if (now - tHb >= HB_PERIOD_MS) {
    tHb = now;
    heartbeatGonder();
  }
}

// ======================================================
// SETUP
// ======================================================
void setup() {
  Serial.begin(115200);

  crsfSerial.begin(420000, SERIAL_8N1);
  crsf.begin(crsfSerial);

  haberlesmeBaslat(115200);   // [HABERLESME] Jetson UART3 @115200

  analogWriteFrequency(MOTOR_PWM_HZ);

  pinMode(SOL_ON_RPWM, OUTPUT);
  pinMode(SOL_ON_LPWM, OUTPUT);
  pinMode(SOL_ARKA_RPWM, OUTPUT);
  pinMode(SOL_ARKA_LPWM, OUTPUT);
  pinMode(SAG_DIR, OUTPUT);
  pinMode(SAG_PWM, OUTPUT);

  hepsiniDurdur();

  pinMode(ATES_PIN, OUTPUT);
  ateslemeKapat();

  servoBaslat();
  servo1Aci = SERVO_BASLANGIC;
  servo2Aci = SERVO_BASLANGIC;
  servoYaz1(SERVO_BASLANGIC);
  servoYaz2(SERVO_BASLANGIC);
}

// ======================================================
// LOOP
// ======================================================
void loop() {
  crsf.update();
  haberlesmeAl();             // [HABERLESME] RX pompasi - her loop, non-blocking

  static unsigned long sonZaman = 0;
  if (millis() - sonZaman < 20) return;
  sonZaman = millis();

  bool linkUp = crsf.isLinkUp();

  // ---- CRSF FAILSAFE (RC kopus) ----
  if (!linkUp) {
    hepsiniDurdur();
    ateslemeKapat();
    g_failsafe = true;
    g_solUyg = 0; g_sagUyg = 0;
    haberlesmePeriyodik(false, false);  // [HABERLESME] Jetson yine de durumu gorsun
    return;
  }
  g_failsafe = false;

  // ---- Mod Secimi (orijinal) ----
  int ch5 = crsf.getChannel(5);
  int istenenMod = aktifMod;
  if (ch5 < MOD_SURUS_ESIK)      istenenMod = MOD_SURUS;
  else if (ch5 > MOD_LAZER_ESIK) istenenMod = MOD_LAZER;

  if (istenenMod != aktifMod) {
    modSayac++;
    if (modSayac >= MOD_ONAY_SAYISI) {
      aktifMod = istenenMod;
      modSayac = 0;
    }
  } else {
    modSayac = 0;
  }

  if (aktifMod != oncekiMod) {
    hepsiniDurdur();
    if (aktifMod == MOD_LAZER) {
#if SERVO_MODA_GIRINCE_ORTALA
      servo1Aci = SERVO_BASLANGIC;
      servo2Aci = SERVO_BASLANGIC;
      servoYaz1(SERVO_BASLANGIC);
      servoYaz2(SERVO_BASLANGIC);
#endif
      ateslemeKapat();
    } else {
      ateslemeKapat();
    }
    oncekiMod = aktifMod;
  }

  // ---- [HABERLESME] Arbitrasyon: MANUEL mi OTONOM mu? ----
  bool otonom = otonomIzinliMi();

  if (otonom) {
    if (aktifMod == MOD_SURUS) otonomSurus();
    else                       otonomLazer();
  } else {
    if (aktifMod == MOD_SURUS) { ateslemeKapat(); surusModu(); }
    else                       lazerModu();
  }

  // ---- [HABERLESME] Periyodik STATUS + HEARTBEAT ----
  haberlesmePeriyodik(linkUp, otonom);
}
