# Nucleo-F072RB firmware kurulumu (native USB-CDC)

`argexika/` (STM32F407 **lazerson**) firmware'inin **Nucleo-F072RB** portu.
Uygulama mantığı (`crsf`, `reactor`, `servo`, tank-mix, **CH9 E-STOP**, **lazer/otonom
şeması**, telemetri) F407 ile **birebir aynı**; yalnız donanım katmanı (clock, USART,
DMA, TIM, GPIO ve **Jetson linki = native USB-CDC**) F072'ye taşındı.

> ⚠️ Bu dosyalar **elde portlandı, henüz donanımda test EDİLMEDİ.** STM32CubeIDE'de
> derleyip araç üstünde (önce **motorlar havadayken**) doğrula.

Pin haritası bu dosyanın sonundadır. F407 haritası: [../pinmodev1.md](../pinmodev1.md).

---

## 0. Neden CubeMX/CubeIDE şart?

Bu klasör yalnızca **uygulama kaynağını** içerir. Bir STM32 projesinin geri kalanını —
F0xx HAL sürücü ağacı, `startup_stm32f072xbtx.s`, linker script, `system_stm32f0xx.c`,
`stm32f0xx_hal_conf.h` ve özellikle **USB Device middleware'i** (`usbd_core`, `usbd_cdc`,
`usbd_conf`, `stm32f0xx_hal_pcd.c` …) — **STM32CubeIDE üretir.** Native USB seçildiği
için bu üretim ZORUNLU: USB alt sürücüleri elle yazılamaz.

STM32CubeIDE zaten kurulu: `C:\ST\STM32CubeIDE_2.2.0`. (F0 firmware paketi ilk
"Generate Code"da otomatik iner — internet gerekir.)

---

## 1. Boş proje oluştur (STM32CubeIDE)

1. **File → New → STM32 Project**
2. **Board Selector → NUCLEO-F072RB** seç (ya da Part Number `STM32F072RBTx`) → **Next**
3. İsim ver (örn. `nucleo_f072_ika`), **C** projesi → Finish.
   (Board seçtiysen "initialize all peripherals in default mode?" çıkarsa **No** de —
   pinleri biz koddan kuruyoruz.)

### 1a. USB Device'ı aç (TEK gerçek CubeMX işi)

`.ioc` sekmesinde:

- **Connectivity → USB** → *Device (FS)* → **Activate** (`USB` işaretlensin).
- **Middleware and Software Packs → USB_DEVICE** →
  *Class For FS IP* = **Communication Device Class (Virtual Port Com)**.

### 1b. Saat: HSI48 → 48 MHz

**Clock Configuration** sekmesinde:

- **SYSCLK / HCLK = 48 MHz**, kaynak **HSI48** (harici kristal YOK).
- **USB clock = 48 MHz** (kaynak HSI48). CubeMX yeşil gösterene kadar ayarla.
- CubeMX "Clock Recovery System (CRS)" önerirse aç; **açmasan da olur** — kodumuz
  (`SystemClock_Config`) CRS'i kendi kuruyor.

> Pin/USART/TIM/DMA'ya **DOKUNMA.** Hepsi `main.c` içinde koddan kuruluyor.

### 1c. Generate Code (Ctrl+S)

İskelet + USB middleware + HAL ağacı üretilir.

---

## 2. HAL modüllerini kontrol et  ⚠️ (derleme için kritik)

Pinleri koddan kurduğumuz için CubeMX bazı HAL modüllerini **kapalı** bırakabilir.
Üretilen **`Core/Inc/stm32f0xx_hal_conf.h`** dosyasını aç, şu satırların **açık
(yorumsuz)** olduğundan emin ol:

```c
#define HAL_UART_MODULE_ENABLED
#define HAL_TIM_MODULE_ENABLED
#define HAL_DMA_MODULE_ENABLED
#define HAL_PCD_MODULE_ENABLED     /* USB - CubeMX zaten açar */
#define HAL_RCC_MODULE_ENABLED     /* CRS bunun içinde */
#define HAL_GPIO_MODULE_ENABLED
#define HAL_CORTEX_MODULE_ENABLED
```

Kapalıysa `//` işaretini kaldır. (`UART`, `TIM`, `DMA` genelde CubeMX'te ilgili
çevre birim açılmadığı için kapalı gelir — asıl bunları aç.)

---

## 3. Bu klasördeki dosyaları projeye kopyala

Üretilen dosyaların **üzerine** kopyala / ekle:

| Kaynak (bu repo `nucleo-f072/`) | Hedef (CubeIDE projesi) | İşlem |
|---|---|---|
| `Core/Src/main.c`            | `Core/Src/main.c`            | **DEĞİŞTİR** |
| `Core/Inc/main.h`            | `Core/Inc/main.h`            | **DEĞİŞTİR** |
| `Core/Src/stm32f0xx_it.c`    | `Core/Src/stm32f0xx_it.c`    | **DEĞİŞTİR** |
| `Core/Inc/stm32f0xx_it.h`    | `Core/Inc/stm32f0xx_it.h`    | **DEĞİŞTİR** |
| `USB_DEVICE/App/usbd_cdc_if.c` | `USB_DEVICE/App/usbd_cdc_if.c` | **DEĞİŞTİR** (RX kancası içinde) |
| `Core/Src/crsf.c reactor.c servo.c haberlesme.c` | `Core/Src/` | **EKLE** |
| `Core/Inc/crsf.h reactor.h servo.h haberlesme.h` | `Core/Inc/` | **EKLE** |

> **`stm32f0xx_it.c`'yi neden komple değiştiriyoruz?** İçinde `USB_IRQHandler`
> (USB kesmesi) + `DMA1_Channel2_3_IRQHandler` (CRSF DMA) var; CubeMX'in ürettiği
> `SysTick`/`USB` handler'larıyla çakışmasın diye tamamını değiştiriyoruz.

> **`usbd_cdc_if.c` alternatifi:** Komple değiştirmek istemezsen, CubeMX'in ürettiği
> dosyada 3 küçük ekleme yeter (aşağıda §3a).

### 3a. (Alternatif) usbd_cdc_if.c'yi elle yamalamak

Üretilen `USB_DEVICE/App/usbd_cdc_if.c` içinde:

```c
/* USER CODE BEGIN INCLUDE */
#include "haberlesme.h"                 /* <-- ekle */
/* USER CODE END INCLUDE */
```
```c
static int8_t CDC_Receive_FS(uint8_t* Buf, uint32_t *Len)
{
  /* USER CODE BEGIN 6 */
  Haberlesme_CdcRxPush(Buf, *Len);      /* <-- ekle (ilk satir) */
  USBD_CDC_SetRxBuffer(&hUsbDeviceFS, &Buf[0]);
  USBD_CDC_ReceivePacket(&hUsbDeviceFS);
  return (USBD_OK);
  /* USER CODE END 6 */
}
```
```c
uint8_t CDC_Transmit_FS(uint8_t* Buf, uint16_t Len)
{
  uint8_t result = USBD_OK;
  /* USER CODE BEGIN 7 */
  USBD_CDC_HandleTypeDef *hcdc = (USBD_CDC_HandleTypeDef*)hUsbDeviceFS.pClassData;
  if (hcdc == NULL){ return USBD_FAIL; }   /* <-- ekle: host yokken null deref koru */
  if (hcdc->TxState != 0){ return USBD_BUSY; }
  ...
```

### 3b. (İsteğe bağlı) RAM tasarrufu — 16 KB SRAM

`USB_DEVICE/App/usbd_cdc_if.h` içindeki tamponlar varsayılan 2048'er bayttır (4 KB).
İstersen küçült (protokol paketleri ≤ ~20 bayt):

```c
#define APP_RX_DATA_SIZE  512
#define APP_TX_DATA_SIZE  512
```

---

## 4. Derle ve yükle

- **Project → Build** (Ctrl+B). Hata yoksa `.elf` üretilir.
- Nucleo'yu **onboard USB (ST-Link)** ile PC'ye tak → **Run** (yeşil ok).

### Derlemede çıkabilecek olası sorunlar

| Belirti | Sebep / çözüm |
|---|---|
| `HAL_UART_Init` / `HAL_TIM_*` "undefined" | §2'deki HAL modülleri kapalı → aç. |
| `GPIO_AF4_USART3` / `GPIO_AF0_USART4` bulunamadı | HAL sürüm farkı → datasheet AF no'sunu yaz (USART3=`4`, USART4=`0`, USART1=`1`, TIM3=`1`). |
| `hpcd_USB_FS` undefined (it.c) | CubeMX PCD handle adı farklı olabilir → `usbd_conf.c`'deki `PCD_HandleTypeDef hpcd_USB_FS;` adına göre `stm32f0xx_it.c`'yi düzelt. |
| `RCC_USBCLKSOURCE_HSI48` / CRS makroları yok | USB, `.ioc`'de açılmamış (§1a) ya da HSI48 seçili değil (§1b). |

---

## 5. USB kablolaması (Jetson linki)  ⚠️ ÖNEMLİ

Nucleo'nun **onboard USB soketi ST-Link'e** gider, MCU'nun kendi USB'sine **değil**.
Native USB-CDC için MCU'nun USB pinlerini **ayrı bir USB kablosuna** bağlaman gerekir:

```
STM32 PA12 (USB_DP, D+)  ───────►  USB konnektor D+   ─┐
STM32 PA11 (USB_DM, D-)  ───────►  USB konnektor D-    ├─► Jetson USB portu
STM32 GND                ───────►  USB konnektor GND  ─┘
        (USB konnektorun VBUS/5V hattini STM32'ye BAGLAMA — sadece D+/D-/GND)
```

- Harici pull-up gerekmez (F072'nin dahili DP pull-up'ı var).
- Onboard USB (ST-Link) flashlama/debug için ayrıca takılı kalabilir.
- Jetson'da **`/dev/ttyACM*`** olarak görünür (ST VID/PID, CDC-ACM).

---

## 6. Bring-up testi (sırayla, motorlar HAVADA!)

1. **CRSF:** Kumandayı aç → mavi LED (**PC2**) yanmalı (link OK). Kapat → kırmızı (**PC1**).
2. **Servolar:** LAZER moduna al (**CH7 > 1700µs**) → CH1/CH2 stickleri PB0/PB1'de PWM,
   pan/tilt hareket.
3. **Motorlar (HAVADA!):** Sürüş modu (CH7 kapalı), CH5 MANUEL + stick → reactor'lar
   dönmeli. Ters dönerse `main.c`'deki `*_INV_*` işaretini çevir.
4. **E-STOP:** CH9 butonuna bir kez bas → her şey durup **kilitlenir**; reset:
   butonu bırak **ve** CH5'i MANUEL'e al.
5. **Jetson linki (native USB):** PA11/PA12'yi Jetson'a bağla →
   ```bash
   ls -l /dev/serial/by-id/          # ttyACM yolunu bul
   python3 sim/jetson_link_test.py /dev/ttyACM0
   ```
   `jetson_link=True` ve STATUS akışı görülmeli.

Jetson tarafı: [jetson/config.yaml](../jetson/config.yaml) `serial_port` → çıkan
`by-id` yolunu yaz; `sudo systemctl disable --now ModemManager`; kullanıcı `dialout`'ta.

---

## 7. PİN HARİTASI — Nucleo-F072RB (native USB-CDC)

| İşlev | Pin | Çevre birim | AF | Baud/PWM | Not |
|---|---|---|---|---|---|
| **Jetson (USB-CDC)** | **PA11/PA12** | USB (DM/DP) | — | FS | ayrı USB kablosu → Jetson |
| **CRSF / ELRS** | **PA10** | USART1_RX | 1 | 420000 | DMA1 Ch3 (circular) |
| **SOL Reactor** | **PB10** | USART3_TX | 4 | 38400 | TX-only |
| **SAĞ Reactor** | **PC10** | USART4_TX | 0 | 38400 | TX-only |
| **PAN servo** | **PB0** | TIM3_CH3 | 1 | 50 Hz | 700–2300 µs, rate |
| **TILT servo** | **PB1** | TIM3_CH4 | 1 | 50 Hz | 1100–1900 µs, mutlak |
| **Reactor EN** | **PC0** | GPIO out | — | — | armlıyken HIGH |
| **LAZER ateş** | **PC3** | GPIO out | — | — | CH10 tetikte HIGH |
| **Kırmızı LED (FAILSAFE)** | **PC1** | GPIO out | — | — | link yok/E-STOP |
| **Mavi LED (LINK OK)** | **PC2** | GPIO out | — | — | link var |

### F407 → F072 değişen donanım

| İşlev | F407 (lazerson) | F072 (bu port) |
|---|---|---|
| Jetson | USB-CDC OTG_FS PA11/PA12 | **USB-CDC (USB IP) PA11/PA12** |
| CRSF | USART2 PA3, DMA1_Stream5 | **USART1 PA10, DMA1_Ch3** |
| SOL Reactor | USART1 PB6 | **USART3 PB10** |
| SAĞ Reactor | USART3 PD8 | **USART4 PC10** |
| Servo PAN/TILT | TIM4 PD12/PD13 | **TIM3 PB0/PB1** |
| EN / ATEŞ / LED | PE0 / PE1 / PD14-PD15 | **PC0 / PC3 / PC1-PC2** |
| Clock | 168 MHz (HSE+PLL) | **48 MHz (HSI48 + CRS)** |

### Kumanda kanalları (F407 lazerson ile AYNI)

| Kanal | Sürüş modu | Lazer modu | Eşik |
|---|---|---|---|
| CH1 | STEER | PAN servo | stick |
| CH2 | THROTTLE | TILT servo | stick |
| CH5 | MANUEL/OTONOM | — | >1500 = OTONOM |
| CH6 | HIZ (%30/60/100) | — | 3 poz |
| CH7 | — | LAZER modu | >1700 = LAZER |
| CH9 | E-STOP (kilitli) | E-STOP | >1500 = basılı |
| CH10 | — | ATEŞ | >1700 = ateş |

**Öncelik:** CH7 (lazer) > CH5. Lazer açıkken motorlar durur, otonom devre dışı.
Otonom sürüş şu an yalnız bayrak gönderir (`main.c` → `AUTO_DRIVE_ENABLED 0`); Jetson
sürüş kodu hazır olunca `1` yap.
