# PIN MODE v2 — Nucleo-F072RB (STM32F072RBTx)

`argexika/` (STM32F407) firmware'inin **Nucleo-F072RB portu** için pin haritası.
Kaynak: [`nucleo-f072/Core/Src/main.c`](nucleo-f072/Core/Src/main.c) +
[`nucleo-f072/Core/Src/haberlesme.c`](nucleo-f072/Core/Src/haberlesme.c).
Önceki (F407) harita: [pinmodev1.md](pinmodev1.md).

> Kart: **NUCLEO-F072RB**, SYSCLK 48 MHz (dahili **HSI48**, harici kristal yok).
> Elde **4 USART** var; aracın 4 seri hattına birebir yetiyor (boşta USART kalmıyor).

---

## 1. UART hatları

| Pin | Yön | İşlev | Çevre birim | AF | Baud | DMA | Nereye gider |
|---|---|---|---|---|---|---|---|
| **PA2** | TX | **Jetson** telemetri/komut | USART2 | AF1 | 115200 8N1 | — | ST-Link VCP (dahili) |
| **PA3** | RX | **Jetson** telemetri/komut | USART2 | AF1 | 115200 8N1 | DMA1 Ch5 | ST-Link VCP (dahili) |
| **PA10** | RX | **CRSF / ELRS alıcı** | USART1 | AF1 | 420000 8N1 | DMA1 Ch3 | ELRS alıcısının TX pedi |
| **PB10** | TX | **SOL Reactor** sürücü | USART3 | AF4 | 38400 8N1 | — | Sol Reactor "SRL" pini |
| **PC10** | TX | **SAG Reactor** sürücü | USART4 | AF0 | 38400 8N1 | — | Sağ Reactor "SRL" pini |

- **Jetson bağlantısı USB üzerinden:** Nucleo'nun ST-Link mini-USB'si → Jetson USB
  portu. USART2 (PA2/PA3) donanımsal olarak ST-Link VCP'ye bağlı (Nucleo SB köprüleri).
  Jetson'da `/dev/ttyACM*` olarak görünür. **Harici kablo yok.**
- CRSF DMA: **DMA1 Channel3** (USART1_RX), circular → `DMA1_Channel2_3_IRQn`.
- Jetson DMA: **DMA1 Channel5** (USART2_RX), circular → `DMA1_Channel4_5_6_7_IRQn`.
- Reactor'lar TX-only, blocking (`HAL_UART_Transmit`), DMA yok.

---

## 2. Servo / Taret (TIM3 PWM, 50 Hz)

TIM3 1 MHz'e ayarlı (PSC=48-1 @48MHz), ARR=20000-1 → 50 Hz. CCR değeri doğrudan µs.

| Pin | İşlev | Timer kanalı | AF | Aralık (µs) | Merkez | Mod |
|---|---|---|---|---|---|---|
| **PB0** | **PAN** (yatay) servo | TIM3_CH3 | AF1 | 700 – 2300 | 1500 | rate (400 µs/s) |
| **PB1** | **TILT** (dikey) servo | TIM3_CH4 | AF1 | 1100 – 1900 | 1500 | mutlak (konum) |

- Servo: RDS3235 (~270°, 500–2500 µs fiziksel sınır).

---

## 3. GPIO (dijital çıkış)

| Pin | İşlev | Aktif durum |
|---|---|---|
| **PC0** | Reactor **EN** (enable / röle) | ARM'lı ve link varken HIGH |
| **PC3** | **LAZER** atesleme çıkışı | LAZER modunda CH10>1700µs iken HIGH |
| **PC1** | **Kırmızı LED** = FAILSAFE | Link **yok** iken yanar |
| **PC2** | **Mavi LED** = LINK OK | Link **var** iken yanar |

---

## 4. Kumanda kanalları (CRSF) — F407 ile AYNI

| Kanal | Fonksiyon |
|---|---|
| **CH1** STEER | Tank-mix dönüş |
| **CH2** THROTTLE | Tank-mix ileri/geri |
| **CH3** TILT | Dikey eksen servo |
| **CH4** PAN | Yatay eksen servo |
| **CH5** ARM/MOD | >1700µs = SÜRÜŞ (armed); ≤1700µs = LAZER modu |
| **CH6** HIZ LİMİTİ | 3 pozisyon (%30/%60/%100) |
| **CH7** TARET MERKEZ | >1700µs = pan/tilt merkeze |
| **CH10** ATEŞ | >1700µs = ateş (yalnız LAZER modunda) |

---

## 5. F407 → F072 pin taşıma özeti

| İşlev | F407 (pinmodev1) | F072 (bu dosya) |
|---|---|---|
| Jetson linki | USART6 PC6/PC7 | **USART2 PA2/PA3 (VCP)** |
| CRSF | USART2 PA3 | **USART1 PA10** |
| SOL Reactor | USART1 PB6 | **USART3 PB10** |
| SAG Reactor | USART3 PD8 | **USART4 PC10** |
| Servo PAN/TILT | TIM4 PD12/PD13 | **TIM3 PB0/PB1** |
| EN | PE0 | **PC0** |
| Ateş | PE1 | **PC3** |
| LED kırmızı/mavi | PD14/PD15 | **PC1/PC2** |

---

## 6. Notlar / uyarılar

- **Port D/E yok:** F072RB (LQFP64) pratikte sadece PD2 verir, Port E hiç yok. F407'de
  Port D/E'de olan servolar/reactor/LED/EN hepsi Port B/C'ye taşındı.
- **USART6 / TIM4 yok:** F072'de bu çevre birimler bulunmaz; USART2/USART3/USART4 ve
  TIM3 kullanıldı.
- **USART2 = VCP kilidi:** Jetson linki USART2'de olmak ZORUNDA (ST-Link VCP donanımsal
  buraya bağlı). Bu yüzden CRSF USART1'e taşındı.
- Tüm donanım kurulumu **koddan** yapılıyor (`.ioc`'de pin işaretlemeye gerek yok).
- Kurulum adımları: [nucleo-f072/KURULUM.md](nucleo-f072/KURULUM.md).
