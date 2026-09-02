# PIN MODE v1 — STM32F407G-DISC1 (native firmware)

Bu dosya, `argexika/` native firmware'inin **kullandığı tüm pinleri** ve her birinin
nereye gittiğini listeler. Kaynak: [`argexika/Core/Src/main.c`](argexika/Core/Src/main.c),
[`argexika/Core/Src/stm32f4xx_hal_msp.c`](argexika/Core/Src/stm32f4xx_hal_msp.c) ve
[`argexika/Core/Src/haberlesme.c`](argexika/Core/Src/haberlesme.c).

> Kart: **STM32F407G-DISC1**, SYSCLK 168 MHz (8 MHz HSE bypass / ST-LINK MCO).

---

## 1. UART hatları

| Pin | Yön | İşlev | Çevre birim | AF | Baud | Nereye gider |
|---|---|---|---|---|---|---|
| **PB6** | TX | **Sol Reactor** sürücü | USART1 | AF7 | 38400 8N1 | Sol Reactor "SRL" pini |
| **PA3** | RX | **CRSF / ELRS alıcı** | USART2 | AF7 | 420000 8N1 | ELRS alıcısının TX pedi (DMA circular) |
| **PD8** | TX | **Sağ Reactor** sürücü | USART3 | AF7 | 38400 8N1 | Sağ Reactor "SRL" pini |
| **PC6** | TX | **Jetson telemetri** | USART6 | AF8 | 115200 8N1 | Jetson 40-pin **Pin 10 (UART RX)** |

- CRSF DMA: **DMA1_Stream5, Channel 4** (USART2_RX), circular.
- Telemetri **tek yönlü**: sadece PC6 → Jetson RX + GND. RX kablolanmaz.

---

## 2. Servo / Taret (TIM4 PWM, 50 Hz)

TIM4 1 MHz'e ayarlı (PSC=84-1), ARR=20000-1 → 50 Hz. CCR değeri doğrudan mikrosaniye.

| Pin | İşlev | Timer kanalı | AF | Aralık (µs) | Merkez | Mod |
|---|---|---|---|---|---|---|
| **PD12** | **PAN** (yatay) servo | TIM4_CH1 | AF2 | 700 – 2300 | 1500 | rate (400 µs/s) |
| **PD13** | **TILT** (dikey) servo | TIM4_CH2 | AF2 | 1100 – 1900 | 1500 | mutlak (konum) |

- Servo: RDS3235 (~270°, 500–2500 µs fiziksel sınır).

---

## 3. GPIO (dijital giriş/çıkış)

| Pin | Yön | İşlev | Aktif durum |
|---|---|---|---|
| **PE0** | Çıkış | Reactor **EN** (enable / röle) | Manuel/otonom sürüşte kontrol geçerliyken HIGH; lazer/E-STOP/failsafe'te LOW |
| **PE1** | Çıkış | **Lazer ateşleme** | Lazer modu + CH10 tetikte HIGH (aksi LOW) |
| **PD14** | Çıkış | **Kırmızı LED** = FAILSAFE | Link **yok** iken yanar |
| **PD15** | Çıkış | **Mavi LED** = LINK OK | Link **var** iken yanar |

---

## 4. Kumanda kanalları (CRSF, 1-tabanlı)

> **CH1/CH2 PAYLAŞIMLI:** sürüş modunda motor (dönüş/gaz), lazer modunda taret
> (pan/tilt). Lazer modunda motorlar kilitlendiği için aynı stickler taret için
> kullanılır.

| Kanal | Sürüş modu | Lazer modu | Eşik (µs) |
|---|---|---|---|
| **CH1** | STEER (dönüş) | **PAN** servo | stick |
| **CH2** | THROTTLE (ileri/geri) | **TILT** servo | stick |
| CH3, CH4 | — (kullanılmıyor) | — | — |
| **CH5** | **MANUEL / OTONOM** seçici | — | **>1500 = OTONOM** |
| **CH6** | HIZ LİMİTİ (3 poz %30/%60/%100) | — | — |
| **CH7** | — | **LAZER modu aç** (motorlar kilitlenir) | **>1700 = LAZER** |
| **CH9** | **FAILSAFE** buton → kilitli E-STOP | aynı | **>1500 = basılı** |
| **CH10** | — | **ATEŞ** | **>1700 = ateş** |

- **Öncelik:** CH7 (lazer) > CH5 (manuel/otonom). Lazer açıkken motorlar durur,
  otonom devre dışı kalır.
- **E-STOP:** CH9 anlık butona bir kez basınca motor + lazer **anında durur ve
  KİLİTLENİR**. Reset: butonu bırak **ve** CH5'i **MANUEL** konuma al.
- **Otonom sürüş:** şu an yalnız mod bayrağı gönderilir, motorlar **durur**
  (`main.c` → `AUTO_DRIVE_ENABLED 0`). Jetson sürüş kodu hazır olunca `1` yapılınca
  `solHedef/sagHedef` motorlara uygulanır (AUTO_REQ + taze komut el sıkışmasıyla).

---

## 5. STM32 ↔ Jetson kablolama (2 tel)

```
STM32  PC6 (USART6 TX)  ───────────►  Jetson 40-pin  Pin 10 (UART RXD)
STM32  GND             ───────────►  Jetson 40-pin  Pin 6  (GND)
```

- Baud **115200 8N1**, 3.3V. ⚠️ Jetson pinine **asla 5V verme**.
- STM = TX (çıkış), Jetson = RX (giriş). TX→RX olacak şekilde bağla.
- Telemetri çerçevesi: `AA 55 | VER | TYPE | LEN | SEQ | PAYLOAD | CRC16` — base
  station parser'ıyla (`needtocheck/jetson_parser.py`) birebir aynı.

---

## 6. Notlar / uyarılar

- **Değişen pin:** Eski Arduino firmware'inde telemetri **PB6**'daydı. Native
  firmware'de PB6 artık **Sol Reactor**'a gidiyor; telemetri **PC6 (USART6)**'ya taşındı.
- MSP dosyasında **PA10** (USART1_RX), **PA2** (USART2_TX) ve **PB11** (USART3_RX)
  pinleri de yapılandırılmış ama ilgili UART'lar tek yönlü (TX-only / RX-only)
  çalıştığı için bu pinler **kullanılmıyor** (boşta).
- Telemetri UART'ı (USART6/PC6) donanımı **koddan** (`haberlesme.c` içinde) kuruluyor,
  `.ioc` dosyasında tanımlı **değil**. CubeMX'ten yeniden üretim yaparsan bunu göz önünde tut.
- Reactor sürücü dipswitch: **Mode 3 / UART** (1↑ 2↑ 3↓), 38400 8N1.
