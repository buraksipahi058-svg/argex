/*
  =====================================================================
  STM32F407G-DISC1 + 2x REACTOR V1.2 (UART) - TANK DRIVE   [SURUM 4 + TELEMETRI]
  =====================================================================
  Bu dosya, orijinal SURUM 4 Reactor firmware'ine BASE STATION TELEMETRISI
  eklenmis halidir. Eklenen her satir  // [TELEMETRI]  ile isaretlidir.

  ENTEGRASYON OZETI (protokol AYNEN korunur):
    * Jetson'a STATUS(20Hz) + HEARTBEAT(10Hz) gonderimi eklendi.
    * Protokol modulu: haberlesme.h/.cpp (needtocheck/ kopyasi; sadece pin
      tanimi degistirildi -> USART1, TX=PB6, RX=PB7).
    * TEK YONLU: sadece PB6 -> Jetson RX + GND. STM Jetson'dan komut ALMAZ
      (bu firmware manuel-only; Base Station read-only).
    * Bu firmware'de Jetson komutu/otonomi olmadigi icin durum bitlerinden
      yalnizca FAILSAFE uretilir; AUTO_EN/CMD_TIMEOUT/JETSON_LINK/CRC_ERR = 0.
      Base Station bu yuzden "MANUAL" ve otonomi kapali gosterir (dogru davranis).

  Base Station tarafinda (jetson gateway / proto / backend / frontend)
  HICBIR degisiklik gerekmez; cunku tel uzerindeki protokol birebir aynidir.

  ---------------------------------------------------------------------
  LED ANLAMLARI (kart uzerindeki 4 LED)
  ---------------------------------------------------------------------
   MAVI    (PD15) : Heartbeat. Yanip sonuyorsa kod calisiyor.
   KIRMIZI (PD14) : CRSF hattina ham byte geliyor mu?
   YESIL   (PD12) : Gecerli CRSF paketi cozuluyor mu (isLinkUp)?
   TURUNCU (PD13) : Motorlara sifirdan farkli komut gonderiliyor.
  ---------------------------------------------------------------------

  BAGLANTI HARITASI
  ---------------------------------------------------------------------
   SOL Reactor : STM32 PA2 (USART2 TX) -> SRL   + GND (her iki toprak pini)
       Motor A cikisi = SOL ON motor
       Motor B cikisi = SOL ARKA motor
   SAG Reactor : STM32 PC6 (USART6 TX) -> SRL   + GND (her iki toprak pini)
       Motor A cikisi = SAG ON motor
       Motor B cikisi = SAG ARKA motor

   Dipswitch (her iki kart): 1 YUKARI, 2 YUKARI, 3 ASAGI  (Mode 3 / UART)
   Ayardan sonra bataryayi sokup takin. Durum LED'leri BEYAZ yanmali.

   CRSF Alici (asagidaki CRSF_PIN_SECIMI ile belirlenir):
     Secim 1 -> Alici TX = PB11 (RX) , Alici RX = PB10 (TX)
     Secim 2 -> Alici TX = PD9  (RX) , Alici RX = PD8  (TX)
     Alici GND -> STM32 GND        Alici VCC -> 5V

   [TELEMETRI] JETSON  : STM32 PB6 (USART1 TX) -> Jetson RX ,  STM32 GND -> Jetson GND
  =====================================================================
*/

#include <Arduino.h>
#include <AlfredoCRSF.h>
#include "haberlesme.h"          // [TELEMETRI] STM32<->Jetson protokolu (frozen kopya)

// =====================================================================
// AYARLAR
// =====================================================================

// --- MOTOR YON DUZELTMESI --------------------------------------------
//  Bir motor ters donuyorsa ilgili satiri 1 yapin, duzse 0 birakin.
//  Mevcut durum: on motorlar mekanik olarak ters monte edilmis.
#define TERS_SOL_ON       1     // Sol surucu, Motor A cikisi
#define TERS_SOL_ARKA     0     // Sol surucu, Motor B cikisi
#define TERS_SAG_ON       1     // Sag surucu, Motor A cikisi
#define TERS_SAG_ARKA     0     // Sag surucu, Motor B cikisi

//  Aracin TAMAMI ters gidiyorsa (ileri derken geri) bunu 1 yapin.
#define TERS_TUM_ARAC     0

// --- CRSF alici pin secimi -------------------------------------------
//  1 = USART3 / PB11(RX) + PB10(TX)
//  2 = USART3 / PD9(RX)  + PD8(TX)
#define CRSF_PIN_SECIMI   1

// --- CRSF baud --------------------------------------------------------
#define CRSF_BAUD         420000    // Calismazsa 400000 deneyin

// --- Kumandayi devre disi birakan test modu ---------------------------
//  1 = kumanda okunmaz, ileri/dur/geri dongusu. TEKERLEKLER HAVADA OLSUN.
#define TEST_MODU         0

// --- Servo / lazer modulu ---------------------------------------------
#define SERVO_AKTIF       0

// --- Kumanda kanallari -------------------------------------------------
#define KANAL_DONUS       1    // Saga/sola
#define KANAL_GAZ         2    // Ileri/geri
#define KANAL_MOD         5    // Surus / Lazer secici
#define KANAL_ATES        3

// --- Surus ayarlari ----------------------------------------------------
#define SURUCU_BAUD       38400   // Reactor kilavuzunda sabit
#define MAX_GUC           50      // Maksimum motor gucu (%)
#define DEADZONE          5       // Kumanda olu bolgesi (%)

// =====================================================================
// UART TANIMLARI
// =====================================================================
#if CRSF_PIN_SECIMI == 1
  HardwareSerial crsfSerial(PB_11, PB_10);
#else
  HardwareSerial crsfSerial(PD_9, PD_8);
#endif
AlfredoCRSF crsf;

HardwareSerial solSurucuSerial(PA_3, PA_2);   // USART2, TX = PA2
HardwareSerial sagSurucuSerial(PC_7, PC_6);   // USART6, TX = PC6

// !!! Serial.begin() BU KODDA ASLA CAGRILMAZ - USART2 ile catisir !!!
// [TELEMETRI] jetsonSerial (USART1, PB6/PB7) haberlesme.cpp icinde tanimli.

// =====================================================================
// [TELEMETRI] JETSON'A GONDERILEN DURUM + YARDIMCI GLOBALLER
// =====================================================================
VehicleState arac;                 // [TELEMETRI] STATUS kaynagi (haberlesme.h)
int  g_solUyg   = 0;               // [TELEMETRI] uygulanan SOL taraf hizi (MAX_GUC sonrasi)
int  g_sagUyg   = 0;               // [TELEMETRI] uygulanan SAG taraf hizi
bool g_failsafe = false;           // [TELEMETRI] link yok -> failsafe
#if SERVO_AKTIF
bool g_lazerAcik = false;          // [TELEMETRI] atesleme cikisi durumu
#endif

// =====================================================================
// SERVO (istege bagli)
// =====================================================================
#if SERVO_AKTIF
  #include <Servo.h>
  #define SERVO1_PIN        PB0
  #define SERVO2_PIN        PB1
  #define ATES_PIN          PE7
  #define SERVO_MIN_ACI     30
  #define SERVO_MAX_ACI     150
  #define SERVO_BASLANGIC   90
  #define SERVO_HIZ         1.2
  #define SERVO_DEADZONE    10
  #define ATES_ESIK         1500
  Servo servo1, servo2;
  float servo1Aci = SERVO_BASLANGIC;
  float servo2Aci = SERVO_BASLANGIC;
#endif

// =====================================================================
// MOD SECIMI
// =====================================================================
#define MOD_SURUS         0
#define MOD_LAZER         1
#define MOD_SURUS_ESIK    1200
#define MOD_LAZER_ESIK    1800
#define MOD_ONAY_SAYISI   7

int aktifMod  = MOD_SURUS;
int oncekiMod = MOD_SURUS;
int modSayac  = 0;

// =====================================================================
// LED PINLERI
// =====================================================================
#define LED_YESIL     PD12
#define LED_TURUNCU   PD13
#define LED_KIRMIZI   PD14
#define LED_MAVI      PD15

unsigned long sonHamVeri   = 0;
unsigned long sonHeartbeat = 0;
bool          hbDurum      = false;

// =====================================================================
// REACTOR PROTOKOLU (kilavuz sayfa 5 ile dogrulandi)
//   Motor A:   1 = ileri tam,  64 = dur, 127 = geri tam
//   Motor B: 128 = ileri tam, 192 = dur, 255 = geri tam
// =====================================================================
uint8_t formatMotorA(int hiz) {
  hiz = constrain(hiz, -100, 100);
  if (hiz == 0) return 64;
  if (hiz  > 0) return map(hiz,  1,  100,  63,   1);
  return          map(hiz, -1, -100,  65, 127);
}

uint8_t formatMotorB(int hiz) {
  hiz = constrain(hiz, -100, 100);
  if (hiz == 0) return 192;
  if (hiz  > 0) return map(hiz,  1,  100, 191, 128);
  return          map(hiz, -1, -100, 193, 255);
}

// ---------------------------------------------------------------------
// Sol ve sag taraf hizlarini, her motorun kendi yon duzeltmesini
// uygulayarak iki surucuye gonderir.
// ---------------------------------------------------------------------
void surucuHizGonder(int solHiz, int sagHiz) {

#if TERS_TUM_ARAC
  solHiz = -solHiz;
  sagHiz = -sagHiz;
#endif

  // Her motor icin kendi yon duzeltmesi
  int solOn   = TERS_SOL_ON   ? -solHiz : solHiz;
  int solArka = TERS_SOL_ARKA ? -solHiz : solHiz;
  int sagOn   = TERS_SAG_ON   ? -sagHiz : sagHiz;
  int sagArka = TERS_SAG_ARKA ? -sagHiz : sagHiz;

  // SOL surucu: A cikisi = Sol On, B cikisi = Sol Arka
  solSurucuSerial.write(formatMotorA(solOn));
  solSurucuSerial.write(formatMotorB(solArka));

  // SAG surucu: A cikisi = Sag On, B cikisi = Sag Arka
  sagSurucuSerial.write(formatMotorA(sagOn));
  sagSurucuSerial.write(formatMotorB(sagArka));

  // Teshis: sifirdan farkli komut varsa turuncu LED yansin
  digitalWrite(LED_TURUNCU, (solHiz != 0 || sagHiz != 0) ? HIGH : LOW);
}

void motorlariDurdur() { surucuHizGonder(0, 0); }

#if TEST_MODU
// Test modu yardimcisi: komutu 20ms'de bir tekrarlar, heartbeat yakar.
void testGonder(int sol, int sag, unsigned long sure) {
  unsigned long bas = millis();
  digitalWrite(LED_YESIL,   (sol > 0) ? HIGH : LOW);   // Ileri
  digitalWrite(LED_KIRMIZI, (sol < 0) ? HIGH : LOW);   // Geri
  while (millis() - bas < sure) {
    surucuHizGonder(sol, sag);
    digitalWrite(LED_MAVI, ((millis() / 150) % 2) ? HIGH : LOW);
    delay(20);
  }
}
#endif

// =====================================================================
// YARDIMCI
// =====================================================================
int yuzdeCevir(int ham) {
  if (ham < 900 || ham > 2100) return 0;      // gecersiz/atanmamis kanal
  int d = map(ham, 1000, 2000, -100, 100);
  d = constrain(d, -100, 100);
  if (abs(d) < DEADZONE) return 0;
  return d;
}

// =====================================================================
// [TELEMETRI] DURUM DOLDUR + PERIYODIK GONDER
//   Alanlarin kaynagi (gercek protokol alanlari):
//     solMotor  <- g_solUyg      sagMotor <- g_sagUyg
//     pan/tilt  <- servo acilari (servo kapaliysa 90 notr)
//     lazer     <- g_lazerAcik   aktifMod <- aktifMod
//     elrsLink  <- crsf.isLinkUp()
//     durum     <- yalnizca ST_FAILSAFE (Jetson komutu/otonomi yok -> digerleri 0)
// =====================================================================
void aracDurumGuncelle() {                                   // [TELEMETRI]
  arac.solMotor = (int8_t)constrain(g_solUyg, -100, 100);
  arac.sagMotor = (int8_t)constrain(g_sagUyg, -100, 100);
#if SERVO_AKTIF
  arac.pan   = (uint8_t)constrain((int)servo1Aci, 0, 180);
  arac.tilt  = (uint8_t)constrain((int)servo2Aci, 0, 180);
  arac.lazer = g_lazerAcik ? 1 : 0;
#else
  arac.pan   = 90;   // servo yok -> notr
  arac.tilt  = 90;
  arac.lazer = 0;
#endif
  arac.aktifMod = (uint8_t)aktifMod;
  arac.elrsLink = crsf.isLinkUp() ? 1 : 0;

  uint8_t f = 0;
  if (g_failsafe) f |= ST_FAILSAFE;   // digerleri 0: bu firmware Jetson'dan komut ALMIYOR
  arac.durum = f;
}

void telemetriPeriyodik() {                                  // [TELEMETRI]
  static unsigned long tStatus = 0, tHb = 0;
  unsigned long now = millis();
  if (now - tStatus >= STATUS_PERIOD_MS) {   // 20 Hz
    tStatus = now;
    aracDurumGuncelle();
    telemetriGonder(arac);
  }
  if (now - tHb >= HB_PERIOD_MS) {           // 10 Hz
    tHb = now;
    heartbeatGonder();
  }
}

// =====================================================================
// TANK DRIVE KARISIMI (standart diferansiyel)
//   sol = gaz + donus ,  sag = gaz - donus
// =====================================================================
void surusModu() {
  int donus = yuzdeCevir(crsf.getChannel(KANAL_DONUS));
  int gaz   = yuzdeCevir(crsf.getChannel(KANAL_GAZ));

  int sol = gaz + donus;
  int sag = gaz - donus;

  int enBuyuk = max(abs(sol), abs(sag));
  if (enBuyuk > 100) {
    sol = sol * 100 / enBuyuk;
    sag = sag * 100 / enBuyuk;
  }

  sol = sol * MAX_GUC / 100;
  sag = sag * MAX_GUC / 100;

  surucuHizGonder(sol, sag);

  g_solUyg = sol;   // [TELEMETRI] uygulanan sol hiz
  g_sagUyg = sag;   // [TELEMETRI] uygulanan sag hiz
}

#if SERVO_AKTIF
void lazerModu() {
  motorlariDurdur();

  int pan  = yuzdeCevir(crsf.getChannel(KANAL_DONUS));
  int tilt = yuzdeCevir(crsf.getChannel(KANAL_GAZ));
  int ates = crsf.getChannel(KANAL_ATES);

  if (abs(pan)  < SERVO_DEADZONE) pan  = 0;
  if (abs(tilt) < SERVO_DEADZONE) tilt = 0;

  servo1Aci = constrain(servo1Aci + (pan  / 100.0) * SERVO_HIZ, SERVO_MIN_ACI, SERVO_MAX_ACI);
  servo2Aci = constrain(servo2Aci + (tilt / 100.0) * SERVO_HIZ, SERVO_MIN_ACI, SERVO_MAX_ACI);

  servo1.write((int)servo1Aci);
  servo2.write((int)servo2Aci);

  bool ates_on = (ates > ATES_ESIK);          // [TELEMETRI]
  digitalWrite(ATES_PIN, ates_on ? HIGH : LOW);
  g_lazerAcik = ates_on;                       // [TELEMETRI] atesleme durumu
  g_solUyg = 0; g_sagUyg = 0;                  // [TELEMETRI] motorlar duruyor
}
#endif

// =====================================================================
// SETUP
// =====================================================================
void setup() {
  pinMode(LED_YESIL,   OUTPUT);
  pinMode(LED_TURUNCU, OUTPUT);
  pinMode(LED_KIRMIZI, OUTPUT);
  pinMode(LED_MAVI,    OUTPUT);

  crsfSerial.begin(CRSF_BAUD, SERIAL_8N1);
  crsf.begin(crsfSerial);

  solSurucuSerial.begin(SURUCU_BAUD);
  sagSurucuSerial.begin(SURUCU_BAUD);

  haberlesmeBaslat(115200);   // [TELEMETRI] Jetson USART1 @115200 (TX=PB6)

#if SERVO_AKTIF
  pinMode(ATES_PIN, OUTPUT);
  digitalWrite(ATES_PIN, LOW);
  servo1.attach(SERVO1_PIN);
  servo2.attach(SERVO2_PIN);
  servo1.write(SERVO_BASLANGIC);
  servo2.write(SERVO_BASLANGIC);
#endif

  delay(300);              // Surucunun UART hattini algilamasi icin
  motorlariDurdur();

  // Acilis onayi: tum LED'ler 3 kez yanip soner
  for (int i = 0; i < 3; i++) {
    digitalWrite(LED_YESIL, HIGH);   digitalWrite(LED_TURUNCU, HIGH);
    digitalWrite(LED_KIRMIZI, HIGH); digitalWrite(LED_MAVI, HIGH);
    delay(150);
    digitalWrite(LED_YESIL, LOW);    digitalWrite(LED_TURUNCU, LOW);
    digitalWrite(LED_KIRMIZI, LOW);  digitalWrite(LED_MAVI, LOW);
    delay(150);
  }

#if TEST_MODU
  // Test modunda ellerinizi cekmeniz icin 5 saniye bekleme
  for (int i = 0; i < 25; i++) { motorlariDurdur(); delay(200); }
#endif
}

// =====================================================================
// LOOP
// =====================================================================
void loop() {

#if TEST_MODU
  // ---- KUMANDASIZ SURUCU TESTI -------------------------------------
  testGonder( MAX_GUC,  MAX_GUC, 3000);   // Ileri  (yesil LED)
  testGonder(       0,        0, 2000);   // Dur
  testGonder(-MAX_GUC, -MAX_GUC, 3000);   // Geri   (kirmizi LED)
  testGonder(       0,        0, 2000);   // Dur
  return;
#endif

  // ---- Ham veri var mi? (crsf.update() tuketmeden once bak) --------
  if (crsfSerial.available() > 0) sonHamVeri = millis();
  crsf.update();
  digitalWrite(LED_KIRMIZI,
    (sonHamVeri != 0 && millis() - sonHamVeri < 500) ? HIGH : LOW);

  // ---- Heartbeat ---------------------------------------------------
  if (millis() - sonHeartbeat >= 250) {
    sonHeartbeat = millis();
    hbDurum = !hbDurum;
    digitalWrite(LED_MAVI, hbDurum ? HIGH : LOW);
  }

  // ---- 50 Hz kontrol dongusu ---------------------------------------
  static unsigned long sonZaman = 0;
  if (millis() - sonZaman < 20) return;
  sonZaman = millis();

  // ---- Link kontrolu -----------------------------------------------
  bool linkVar = crsf.isLinkUp();
  digitalWrite(LED_YESIL, linkVar ? HIGH : LOW);

  if (!linkVar) {
    motorlariDurdur();
#if SERVO_AKTIF
    digitalWrite(ATES_PIN, LOW);
    g_lazerAcik = false;            // [TELEMETRI]
#endif
    g_failsafe = true;              // [TELEMETRI] failsafe aktif
    g_solUyg = 0; g_sagUyg = 0;     // [TELEMETRI] motorlar durdu
    telemetriPeriyodik();           // [TELEMETRI] failsafe durumunu da Jetson'a bildir
    return;
  }
  g_failsafe = false;               // [TELEMETRI] link var -> failsafe temiz

  // ---- Mod secimi (emniyet kilitli) --------------------------------
#if SERVO_AKTIF
  int chMod = crsf.getChannel(KANAL_MOD);
  int istenen;
  if (chMod < 900 || chMod > 2100)  istenen = MOD_SURUS;   // gecersiz -> guvenli taraf
  else if (chMod < MOD_SURUS_ESIK)  istenen = MOD_SURUS;
  else if (chMod > MOD_LAZER_ESIK)  istenen = MOD_LAZER;
  else                              istenen = aktifMod;

  if (istenen != aktifMod) {
    if (++modSayac >= MOD_ONAY_SAYISI) { aktifMod = istenen; modSayac = 0; }
  } else {
    modSayac = 0;
  }

  if (aktifMod != oncekiMod) {
    motorlariDurdur();
    digitalWrite(ATES_PIN, LOW);
    g_lazerAcik = false;            // [TELEMETRI]
    oncekiMod = aktifMod;
  }
#else
  aktifMod = MOD_SURUS;   // Servo kapaliyken her zaman surus modu
#endif

  // ---- Aktif modu calistir -----------------------------------------
  if (aktifMod == MOD_SURUS) {
    surusModu();
  }
#if SERVO_AKTIF
  else {
    lazerModu();
  }
#endif

  // ---- [TELEMETRI] Periyodik STATUS + HEARTBEAT --------------------
  telemetriPeriyodik();             // [TELEMETRI]
}
