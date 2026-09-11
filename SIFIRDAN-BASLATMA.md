# İKA Base Station — Sıfırdan Başlatma Rehberi

Bu rehber, **her şey kapalıyken** sistemi baştan ayağa kaldırmak içindir. Hiç
bilmeyen biri bile takip edebilsin diye her komut ne yapıyor açıklanmıştır.
Bu oturumda gerçekten çalışan adımlara göre yazıldı.

> **En sık 3 hata (peşinen oku):**
> 1. Tarayıcıda **`127.0.0.1` yazma, `localhost` yaz** (Vite sadece onu dinliyor).
> 2. `.env.local` dosyasını **tırnaksız** yaz (`echo "..."` tırnağı dosyaya koyar).
> 3. Komutu **doğru makinede** çalıştır — `[JETSON]` mi `[LAPTOP]` mı, her adımda yazıyor.

---

## 0. Ne nerede çalışır? (önce bunu anla)

İki bilgisayar var:

| Bileşen | Çalıştığı yer | Ne işe yarar |
|---|---|---|
| **Kameralar** (MediaMTX + ffmpeg) | **JETSON** (araçta) | Kameralar fiziksel olarak Jetson'a takılı |
| **Gateway** (ESP telemetri okur) | **JETSON** | ESP seri kablosu Jetson'da *(şu an sorunlu, bkz. Bölüm 6)* |
| **Backend** (telemetri sunucusu) | **LAPTOP** | Jetson'dan gelen telemetriyi panele iletir |
| **Frontend / Panel** | **LAPTOP** | İzlediğin ekran (`http://localhost:5173`) |

Özet: **JETSON = araç tarafı** (kamera + veri kaynağı), **LAPTOP = izleme tarafı**.
Panel hep laptopta açılır. Video ile telemetri **ayrı** yollardan gider; biri
bozuksa diğeri çalışır (yani telemetri olmasa da kameraları görebilirsin).

---

## Güncel değerler (bu kurulum için)

> IP'ler ağdan otomatik (DHCP) alınır ve **değişebilir**. Aşağıdakiler şu anki
> değerler; her açılışta doğrula (nasıl olduğu Bölüm 1'de).

| Ne | Değer |
|---|---|
| Jetson IP | `192.168.0.200` |
| Laptop IP | `192.168.0.218` |
| Jetson'da repo | `/root/argex` (dal: `lazerson`) |
| GitHub repo | `https://github.com/buraksipahi058-svg/argex.git` |

---

## 1. Ağ ve IP'leri bul

1. Jetson ile laptop **aynı Wi-Fi / ağda** olmalı.
2. Laptoptan Jetson'a **SSH** ile bağlan (Jetson'da komut çalıştırmak için):

   Laptopta bir terminal (Git Bash veya PowerShell) aç:
   ```bash
   ssh root@192.168.0.200
   ```
   Parola sorarsa gir. Artık bu terminal **[JETSON]** oturumudur.

3. **[JETSON]** IP'sini öğren:
   ```bash
   hostname -I
   ```
   İlk adres Jetson'ın IP'si. (SSH için kullandığın adresle aynı olmalı.)

4. **[LAPTOP]** IP'sini öğren (laptopun kendi terminalinde):
   ```bash
   ipconfig
   ```
   `192.168.0.x` ile başlayan **IPv4 Address** satırı laptopun IP'si.
   (`192.168.56.x` görürsen o sanal adaptör, onu kullanma.)

> Jetson'a hiç bağlanamıyorsan (SSH takılıyor): aynı ağda değilsiniz ya da
> Jetson'ın IP'si değişmiş demektir. Jetson'a ekran/klavye takıp `hostname -I`
> ile yeni IP'yi öğren.

---

## 2. LAPTOP hazırlığı (repo bilgisayarda yoksa — sadece ilk sefer)

Bu bölüm **bir kez** yapılır. Sonraki açılışlarda atla.

1. Şunları kur (yoksa):
   - **Git** — https://git-scm.com
   - **Node.js 18+** — https://nodejs.org
   - **Python 3.10+** — https://python.org

2. Repoyu indir (laptopun terminali):
   ```bash
   cd /c/Users/burak/Desktop
   ```
   ```bash
   git clone https://github.com/buraksipahi058-svg/argex.git ugv-base-station
   ```
   ```bash
   cd ugv-base-station && git checkout lazerson
   ```

3. Frontend paketlerini kur:
   ```bash
   npm install
   ```

4. Backend için Python ortamı kur:
   ```bash
   python -m venv .venv
   ```
   ```bash
   .venv/Scripts/python -m pip install -r backend/requirements.txt
   ```
   ```bash
   .venv/Scripts/python scripts/gen_proto.py
   ```

> Not: Kamera izlemek için backend şart değil. Backend'i sadece telemetri
> (ESP) çalışınca kullanacaksın.

---

## 3. JETSON hazırlığı (kod güncel mi)

**[JETSON]** oturumunda (Bölüm 1'deki SSH):

```bash
cd /root/argex
```
```bash
git pull origin lazerson
```
> "local changes" hatası verirse (Jetson kopyası sadece çalıştırma için):
> ```bash
> git fetch origin && git checkout -f -B lazerson origin/lazerson && git reset --hard origin/lazerson
> ```

Bağımlılıklar + protobuf (Jetson'da hazır venv var, `python3` değil `.venv/bin/python` kullan):
```bash
.venv/bin/pip install -r jetson/requirements.txt && .venv/bin/python scripts/gen_proto.py
```

---

## 4. JETSON: Kameraları başlat

### 4.1 MediaMTX kurulu mu? (bir kez indir)
Kontrol et:
```bash
ls -l /root/mediamtx
```
**Yoksa** indir (Jetson internete bağlı olmalı):
```bash
cd /root && url=$(wget -qO- https://api.github.com/repos/bluenviron/mediamtx/releases/latest | grep -o 'https://[^"]*linux_arm64\.tar\.gz' | head -1) && wget -q "$url" -O mediamtx.tar.gz && tar xzf mediamtx.tar.gz mediamtx && ls -l /root/mediamtx
```

### 4.2 Küçük config (bir kez oluştur)
Tüm yayın yollarına izin veren minik ayar:
```bash
printf 'paths:\n  all_others:\n' > /root/mm.yml
```

### 4.3 Kamera kimlikleri (neden numara kullanmıyoruz)
`/dev/video0`, `video2` gibi numaralar reboot/çıkar-tak sonrası **kayar**. O yüzden
script kameraları **sabit kimlikle** bulur; sen bir şey ayarlamazsın:

- **turret (C922):** benzersiz seri numarası var → `/dev/v4l/by-id/...C922...`
- **ön/arka (iki C270):** ikisinin **seri numarası aynı** olduğu için ancak
  **fiziksel USB portundan** ayırt edilir → `/dev/v4l/by-path/...:2.2:...` (ön),
  `...:2.1:...` (arka).

> **Önemli:** Ön ve arka C270'leri **hep aynı USB portunda tut.** Portlarını
> değiştirirsen ön/arka yer değiştirir. Fiziksel olarak ön kameran arka portta
> takılıysa, aşağıdaki script'te `2.2` ve `2.1`'i değiştir.

İstersen kameraların görünüp görünmediğini kontrol et:
```bash
v4l2-ctl --list-devices
```
(C922 + iki C270 görünmeli. Görünmüyorsa kamera takılı değil / USB sorunudur.)

### 4.4 Kamera başlatma script'i (repodan gelir)
Kameraları tek komutla başlatan script repoda hazır: `scripts/start_video.sh`.
Kod güncelse (Bölüm 3'te `git pull` yaptıysan) zaten var. Çalıştırılabilir yap
(bir kez):
```bash
chmod +x /root/argex/scripts/start_video.sh
```
Script kameraları **sabit kimlikle** bulur (v4l2-ctl ile): turret = C922 ismi,
ön = USB portu 2.2, arka = USB portu 2.1. Fiziksel ön/arka ters ise script'in
başındaki `FRONT_PORT` / `REAR_PORT` değerlerini değiştir.

### 4.5 Kameraları başlat (her açılışta)
```bash
/root/argex/scripts/start_video.sh
```
Bu: eski MediaMTX/ffmpeg'i durdurur → MediaMTX'i başlatır → 3 kamerayı push eder.
Her kamera için log **boşsa** = iyi. Hata görürsen Bölüm 7'ye bak.

Eski kayıtları da silip temiz başlamak istersen (⚠️ silme geri alınamaz):
```bash
/root/argex/scripts/start_video.sh --temizle
```
(Onay sorar; `--temizle -y` onaysız siler. Yarış kaydını korumak için silme
varsayılan **değil**.)

Kaydetmek (yarış sonu teslim) istersen → **Bölüm 9**.

---

## 5. LAPTOP: Paneli aç

Laptopun **kendi terminalinde** (SSH değil), repo klasöründe:

1. Kamera host'unu ayarla (Jetson IP'si — **tırnaksız!**):
   ```bash
   cd /c/Users/burak/Desktop/ugv-base-station
   ```
   ```bash
   printf 'VITE_CAMERA_HOST=192.168.0.200\n' > .env.local
   ```
   > Jetson IP'si değiştiyse `192.168.0.200` yerine yeni IP'yi yaz.

2. Paneli başlat (ağdaki herkese açık olsun diye `--host`):
   ```bash
   npm run dev -- --host
   ```
   Terminal iki adres yazar; **`http://192.168.0.218:5173`** (laptop LAN IP) paylaşılabilir olandır.

3. **İlk seferde güvenlik duvarı** — başka cihazdan açılması için (Yönetici PowerShell'de bir kez):
   > Başlat → "PowerShell" → sağ tık → **Yönetici olarak çalıştır**:
   ```
   New-NetFirewallRule -DisplayName "Vite Dev 5173" -Direction Inbound -Action Allow -Protocol TCP -LocalPort 5173
   ```

4. Tarayıcıda aç:
   - Laptopta: **`http://localhost:5173`**  ← `127.0.0.1` DEĞİL!
   - Başka cihaz/telefon (aynı ağda): **`http://192.168.0.218:5173`**

**Beklenen:** ÖN / ARKA / TARET karoları canlı. Telemetri "offline" görünür —
normal (backend/ESP çalışmıyor, bkz. Bölüm 6).

---

## 6. Telemetri / ESP (ŞU AN SORUNLU — kamera için gerekmez)

ESP↔Jetson seri hattı şu an çalışmıyor (USB hub `-110` hatası veriyor; kanıt:
iki seri cihaz da aynı hub'da birlikte düşüyor, kameralar ayrı portta sağlam).
**Muhtemel kalıcı çözüm: beslemeli (powered) USB hub** veya seri adaptörleri
doğrudan Jetson portuna almak. Hat düzelince telemetri şöyle açılır:

1. **[JETSON]** ESP portunu bul:
   ```bash
   ls -l /dev/serial/by-id/
   ```
2. Hattı test et (STATUS akmalı, `jetson_link=True` olmalı):
   ```bash
   .venv/bin/python sim/jetson_link_test.py /dev/serial/by-id/<ESP-yolu>
   ```
3. Çalışırsa `jetson/config.yaml`: `source.type: serial`, `serial_port: <ESP-yolu>`,
   `quic.host: 192.168.0.218` (laptop IP).
4. **[JETSON]** gateway: `.venv/bin/python -m jetson.gateway`
5. **[LAPTOP]** backend: `.venv/Scripts/python -m backend.main` → panelde telemetri canlanır.

Detay ve sorun giderme: [otonomicingerekenler.md](otonomicingerekenler.md).

---

## 7. Sorun giderme (bu kurulumda yaşadıklarımız)

| Belirti | Sebep / Çözüm |
|---|---|
| Tarayıcı `localhost:5173`'e girmiyor | `127.0.0.1` yazmışsındır → **`localhost`** yaz. Ya da dev server durmuş → laptopta `npm run dev -- --host` tekrar çalıştır. |
| Kameralar gelmiyor, telemetri offline | Normal olabilir. Kamera için: `.env.local` tırnaksız + doğru Jetson IP mi? Jetson'da `/root/start_video.sh` çalışıyor mu? |
| `start_video.sh` "kamera BULUNAMADI" diyor | O kamera takılı değil ya da beklenen portta değil. `v4l2-ctl --list-devices` ile bak; C270 portu farklıysa script'teki `2.2`/`2.1`'i güncelle. Numaralar (`videoN`) kayması sorun değil — script kimlikle bulur. |
| `.env.local` işe yaramıyor | İçinde tırnak vardır. `cat .env.local` → `VITE_CAMERA_HOST=192.168.0.200` (tırnaksız) olmalı. Düzeltince `npm run dev`'i **yeniden başlat**. |
| Başka cihazdan "unreachable" | (a) Güvenlik duvarı kuralı eklenmemiş (Bölüm 5.3). (b) Öbür cihaz farklı ağda — `192.168.0.x` olduğundan emin ol. |
| ffmpeg `Exit 8` / `400 Bad Request` | MediaMTX o yola izin vermiyor → `/root/mm.yml` (`paths: all_others:`) ile başlatıldığından emin ol. |
| `which mediamtx` boş | MediaMTX kurulu değil → Bölüm 4.1'deki indirme. |
| Kamera `No space left on device` | USB bandı doldu (aynı bus'ta çok kamera). Bir kamerayı kapat ya da çözünürlüğü düşür (`-video_size 424x240`). |
| Seri port `Errno 5` / `-110`, iki port birden ölü | USB hub sorunu. Fişe dokunmadan resetle: <br>`echo '1-2.4' \| sudo tee /sys/bus/usb/drivers/usb/unbind; sleep 2; echo '1-2.4' \| sudo tee /sys/bus/usb/drivers/usb/bind` <br>(`1-2.4` = hub'ın adresi, `lsusb -t` ile doğrula). Tekrar ediyorsa **powered hub** şart. |
| ESP'den 0 bayt | ESP beslenmiyor / firmware yok / GPIO22→adaptör TX-RX ters. Bkz. Bölüm 6. |

---

## 8. Kapatma / yeniden başlatma

**Kapatma:**
- Laptopta `npm run dev` terminalinde **Ctrl+C**.
- Jetson'da: `pkill -f mediamtx; pkill -f ffmpeg`

**Tekrar açarken** (her şey kurulduktan sonra kısa yol):
1. **[JETSON]** SSH → `/root/start_video.sh`
2. **[LAPTOP]** repo klasörü → `npm run dev -- --host`
3. Tarayıcı → `http://localhost:5173`

(IP'ler değişmediyse `.env.local`'a dokunmana gerek yok. Değiştiyse Bölüm 5.1.)

---

## 9. Kayıt (yarış sonu teslim)

Kameraları kaydetmek için **MediaMTX'e kaydettir** — zaten kodlanmış yayını olduğu
gibi diske yazar, **ekstra CPU/USB/Wi-Fi maliyeti yok**. Kayıt yalnız kamera
yayındayken tutulur.

### 9.1 Kayıt klasörü (bir kez)
```bash
sudo mkdir -p /opt/ugv-recordings && sudo chown $USER:$USER /opt/ugv-recordings
```

### 9.2 `mm.yml`'a kaydı ekle (bir kez)
`/root/mm.yml`'ı kayıtlı sürümle değiştir:
```bash
cat > /root/mm.yml <<'EOF'
pathDefaults:
  record: yes
  recordPath: /opt/ugv-recordings/%path/%Y-%m-%d_%H-%M-%S-%f
  recordFormat: fmp4
  recordSegmentDuration: 15m
  recordDeleteAfter: 0
paths:
  all_others:
EOF
```
Sonra kameraları yeniden başlat (script MediaMTX'i de yeniden başlatır):
```bash
/root/start_video.sh
```
Kayıtlar `/opt/ugv-recordings/cam_turret|cam_front|cam_rear/...` altına `.mp4`
olarak düşer. Kontrol:
```bash
ls -R /opt/ugv-recordings/
```

### 9.3 Ne kadar yer kaplar? (1 Mbps ayarıyla)
| Süre | 1 kamera | 3 kamera |
|---|---|---|
| 1 dk | 7.5 MB | 22.5 MB |
| **15 dk** | 112 MB | **~340 MB** |
| 1 saat | 450 MB | ~1.35 GB |

Herhangi bir USB bellek/SD rahat alır. Daha yüksek kalite istersen `start_video.sh`
içindeki `-b:v 1000k` değerini büyüt (ör. `4000k` → ~4 kat yer).

### 9.4 Teslim — tek komutla USB'ye dök
USB diski tak ve mount et, sonra:
```bash
sudo mkdir -p /media/ugv-usb && sudo mount /dev/sda1 /media/ugv-usb
```
```bash
bash /root/argex/scripts/dump_recordings.sh /media/ugv-usb
```
Bu komut kayıtları diske kopyalar, dosya sayısını doğrular, `manifest.txt` yazar,
`sync` eder ve diski **güvenle unmount eder** → çıkar, teslim et. (Diski exFAT
formatla; `sda1` yerine kendi cihazını yaz — `lsblk -f` ile bul.)

### 9.5 Aynı işi base station'daki düğmeden yapmak
SSH açmadan, panelden tek tıkla da dökebilirsin. Jetson'da küçük bir servis
([jetson/dump_service.py](jetson/dump_service.py)) 8099'da dinler; `start_video.sh`
onu zaten başlatıyor (ayrı başlatmak için: `python3 /root/argex/jetson/dump_service.py &`).
Servis istemciden komut almaz — sadece yukarıdaki `dump_recordings.sh`'i çalıştırır.

Kullanımı: panelde **LIVE FEED · CAMERAS** başlığındaki `USB'YE AT` düğmesi.
İlk tık `EMİN?` (yarışta yanlışlıkla basmayı önler), ikinci tık başlatır. Düğmenin
solunda scriptin son satırı görünür: kopyalanıyor → manifest → `TAMAM — N dosya`
(yeşil) ya da hata (kırmızı, ör. USB mount edilmemiş). Servise ulaşılamıyorsa
düğme `USB SERVİSİ YOK` yazıp pasifleşir.

> USB yine de **önce mount edilmiş olmalı** (9.4'teki `mount` komutu) — düğme
> mount etmez, sadece dökümü çalıştırır. Kimlik doğrulaması yoktur; araç LAN'ı
> kapalı olduğu için yeterli, servisi dışarı açık bir ağda çalıştırma.

> USB diski hub'a takacaksan **beslemeli hub** kullan (Bölüm 7'deki `-110`
> sorununun sebebi de bu). Yarış boyunca kayıt **dahili diske** düşer, USB'yi
> sadece en sonda takıp dökmek en güvenlisidir.

---

*Bu rehber çalışan kamera akışına göre yazıldı. Telemetri (ESP) hattı düzelince
Bölüm 6 devreye girer; kalıcı kamera/kayıt kurulumu için ayrıca
[STEP_BY_STEP.md](STEP_BY_STEP.md) ve [README-basestation.md](README-basestation.md).*
