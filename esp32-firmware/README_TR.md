# İKA V2 — Yumuşak yerinde dönüş ve servo filtreleme

Bu paket, son proje dosyanızdaki **ESP32-WROOM-32 / ESP32 Dev Module** kartı ve pin düzeni esas alınarak hazırlanmıştır. STM32'ye yüklenmez. Arduino tarafı motorları, kumandayı, servoları ve lazer rölesini yönetir. Otonom algılama/rota planlama Jetson'da kalır; eklenen Python dosyası ROS 2 ile seri haberleşme köprüsüdür.


## V2 güncellemesi — mevcut çalışan sistem üzerine

Bu sürüm ilk araç testinde bildirilen servo titremesi, tilt üst sınırı ve dönüş sarsıntısı için hazırlanmıştır. Pinler, kumanda kanal atamaları ve Jetson çerçevesi korunmuştur. Yeni kod eski `controller.cpp` ile karıştırılmamalı: ZIP'i ayrı bir klasöre çıkartıp içindeki projeyi aç.

### Dönüşte ne değişti?

Önceki kod ileri/geri ve dönüş komutlarını toplayarak iki tarafa farklı büyüklükte hız komutu veriyordu. Bu normal diferansiyel sürüş yöntemidir; tek başına arıza belirtisi değildir. V2'de senin istediğin **eşit büyüklük, zıt yön** davranışı manuel modda uygulanıyor:

| Sağ joystick | Sol taraftaki iki motor | Sağ taraftaki iki motor |
|---|---|---|
| İleri | +S | +S |
| Geri | −S | −S |
| Sağa | +S | −S |
| Sola | −S | +S |
| Çapraz, sağ/sol dönüş eşiği aşılmış | Dönüş yönüne göre ±S | Sol tarafın zıddı |

Bunlar araç üzerindeki ileri yön referansına göre komutlardır; mevcut dört `INV_*` ayarı motor kablo yönlerini çevirmeye devam eder. **Enkoder olmadığı için aynı gerçek RPM ölçülüp garanti edilemez.** Motor, redüktör, lastik, yük ve sürücü ayarları fiziksel hızları etkileyebilir. Yerinde dönüş lastiklerin zeminde yanal kaymasını gerektirir; dolayısıyla sarsıntıyı her zeminde gidereceği garanti değildir.

Dönüş seçimi yaklaşık %20 joystick yatay hareketinde devreye girer; merkez yönüne yaklaşık %14'e dönünce çıkar. İki ayrı eşik, düz sürüş/dönüş arasında hızlı gidip gelmeyi azaltır. Eşiklerin sayısal değerleri deadband sonrası normalize komutta 180/120'dir.

- Düz sürüşten dönüşe, sağ dönüşten sola veya ileri sürüşten geriye geçerken mevcut komut önce sıfıra iner.
- **150 ms sıfır komut** beklenir, sonra yeni yönde hız artırılır. Bu, fiziksel tekerlek hızının sıfırlandığını ölçmez.
- Hızlanma saniyede **70 yüzde puanı**, yavaşlama **150 yüzde puanı** ile sınırlı. Örneğin %30'a çıkış yaklaşık 0,43 saniye sürer. Joystick bırakıldığında olağan manuel duruş da yumuşar; %100'den sıfır komuta iniş yaklaşık 0,67 saniyedir.
- **Mod değişimi, kumanda kaybı ve hata durumunda bu rampa beklenmez:** önceki sürümdeki STOP komutu doğrudan gönderilir.
- F'nin düz sürüş sınırları %30/%60/%100 olarak kalır. Tam yatay joystickte dönüş sınırları sırasıyla **%18/%35/%35** olur; son ikisi yeni %35 dönüş tavanıyla sınırlanır.
- **Çapraz joystick artık kavis çizdirmez:** yatay dönüş eşiği aşılınca ileri/geri komutu yerine yerinde dönüş seçilir. Joystick yatayda merkez bölgesine gelince ileri/geri sürüşe dönülür, gerekiyorsa yine sıfıra iniş/bekleme uygulanır.
- Bu manuel profil AUTO'ya uygulanmaz. Jetson'un farklı sol/sağ hızlarla kavis çizme komutları korunur.

Ayarlar `config.h`: `MANUAL_TURN_MAX_PCT`, `MANUAL_ACCEL_PCT_PER_S`, `MANUAL_DECEL_PCT_PER_S`, `MANUAL_DIRECTION_HOLD_MS`, `MANUAL_PIVOT_ENTER/EXIT_NORM`. Bu sınırlar tekerlek hız ölçümü değil, sürücüye giden komut sınırlarıdır.

### Servo titremesi için değişiklikler

Servo joystick girişlerine üç örnekli medyan, yumuşatma ve iki eşikli merkez bölgesi eklendi. Hareket başlangıcı ±70 µs, merkezde durma ±45 µs; kısa süreli giriş sıçramaları ve merkez çevresindeki küçük değişimler bastırılır. Joystick merkezdeyken filtre ve kesir birikimi sıfırlanır, konum sürüklenmez.

Servo hedefleri 20 ms aralıkla, en az 2 µs biriktirilmiş manuel konum adımıyla güncellenir. Sabit PWM duty değeri tekrar yazılmaz; **50 Hz donanım PWM devam eder ve servo konum tutma torku korunur**. AUTO servo hedefleri de bir anda atlamak yerine mevcut hız sınırıyla yaklaşır; hedef çevresindeki 3 µs fark tutulur. Sıfırdan büyük fakat küçük joystick komutlarının tepkisi önceki sürüme göre biraz daha yumuşaktır.

Bu değişiklikler komut kaynaklı titremeyi azaltmayı amaçlar. Regülatör dalgalanmasını, motor parazitini veya mekanik yük altında servo salınımını yazılım ortadan kaldıramaz. Servo sabit tutulurken Seri Monitör'deki PAN/TILT komutları da sabit olduğu halde titriyorsa:

1. Ana motor gücü kapalı, yalnız servo regülatörleri ve kontrol kartı açıkken tekrar gözle.
2. Regülatör çıkışını **servo konnektöründe, hareket ve yük sırasında** ölç; multimetre kısa gerilim düşüşlerini kaçırabilir. Beslemeyi servo modelinin izin verdiği aralıkta tut.
3. Gevşek kontak, uzun/ince güç dönüşü, motor güç kablosuna paralel sinyal kablosu ve mekanik sıkışma/boşluk kontrolü yap. Servo yükünü güvenli biçimde destekleyerek yük etkisini ayır.

Dönüş sırasında Reactor ER_A/ER_B ışıkları yanıyorsa akım sınırlaması da kesik kesik harekete neden olabilir. Üretici kılavuzunda bu olasılık açıklanır; yalnız titremeyi bastırmak için korumayı kaldırma veya akım sınırını körlemesine artırma. İki sürücünün ramp/akım ayarlarını karşılaştır; bu ayarlar açılışta okunur.

### Tilt biraz daha yukarı

Mevcut `TILT_INPUT_SIGN=-1` ayarında yukarı komut daha kısa PWM darbesine gider. Bu nedenle yukarı sınırı **1100 → 1050 µs** genişletildi; merkez **1500**, diğer uç **1900 µs** kaldı. `TILT_UP_EXTRA_US=50` ile ayarlanır. Geri almak için 0 yap. Bu, fiziksel açıda “50 derece” artış değildir; gerçek açı servo/mekanik düzenine bağlıdır.

İlk kez üst sınıra yavaş yaklaş; gövdeye temas veya mekanik zorlama varsa hemen geri çek ve ek aralığı azalt. Yönü önceki dosyana göre kendin ters çevirmişsen eski yön ayarlarını yeni config.h'ye taşı: sınır genişletmesi `TILT_INPUT_SIGN` işaretini izler. Aynı şekilde kendi röle tetik polaritesi, motor yönü veya CH8 işareti değişikliklerin varsa bunları koru; burada yüklediğin orijinal paketin ayarları esas alındı.

### Yüklemeden sonraki kısa kontrol

İlk düşük hızlı denemeyi sabitlenmiş araç ve havadaki tekerleklerle yap. İleri → sağ → sol → geri geçişinde aradaki sıfır komut beklemesini gör. SERVO_LASER'e geçince tüm ana motorların durduğunu ve kumandayı kapatınca rampa beklenmeden STOP gönderildiğini doğrula. Ardından düşük F konumunda zeminde dene; lastik sürtünmesi ancak bu aşamada görülebilir.

## 1. Paketi aç ve yükle

1. ZIP'i tamamen çıkart. `ika_esp32/ika_esp32.ino` dosyasını aç. Aynı klasördeki `controller.cpp` ve `config.h` birlikte kalmalı. INO dosyasının kısa olması normaldir: uygulama controller.cpp içindedir.
2. Arduino IDE > Tercihler > Ek Kart Yöneticisi URL'leri alanına `https://espressif.github.io/arduino-esp32/package_esp32_index.json` ekle.
3. Kart Yöneticisi'nden **esp32 by Espressif Systems — 3.3.0** kur. Bu sürümle gerçek derleme yapıldı. 2.x desteklenmiyor.
4. Araçlar > Kart > **ESP32 Dev Module** seç. Upload Speed: **115200**. Diğer ayarlar varsayılan kalabilir. ESP32 USB'sinin COM portunu seç; USB-TTL dönüştürücüsünün COM portunu seçme.
5. Motor, servo ve lazer güçleri kapalıyken ESP32'yi USB'den bağla ve Yükle'ye bas. `Connecting...` durumunda gerekirse BOOT'u basılı tut, yükleme başlayınca bırak.
6. Seri Monitör: **115200 baud**. `LINK`, `MODE`, CH değerleri ve çıkış komutlarını izle.

Ek Servo/SoftwareSerial/CRSF kütüphanesi kurmak gerekmiyor. İki motor seri hattının zamanlaması ESP32 RMT donanımıyla üretilir. USB üzerinden teşhis, ELRS ve Jetson donanım UART'larını kullanır.

## 2. Kumanda düzeni

Fiziksel anahtarın yukarı/aşağı konumu tek başına kanalın sayısal yönünü belirlemez. TX12 kanal monitöründe aşağıdaki değerleri elde et; gerekiyorsa ilgili kanalın Outputs yönünü ters çevir.

| Giriş | Kanal | Ayar / davranış |
|---|---|---|
| Sağ joystick yatay | CH1 | Sağ +100, sol −100; manuel sürüşte dönüş, servo modunda pan |
| Sağ joystick dikey | CH2 | İleri/yukarı +100, geri/aşağı −100; sürüş veya tilt |
| E | CH5 | +100: MANUAL; −100: SERVO_LASER |
| F | CH6 | −100: %30, 0: %60, +100: %100 manuel motor komut sınırı |
| C | CH8 | **En alt konum +100: AUTO**; diğer konumlar −100 veya 0 |
| D | CH10 | Basılı/aktif +100: lazer açık; bırakılmış/pasif −100: kapalı |

F iki konumluysa yalnızca %30 ve %100 seçilir; %60 için kanalın 0 üreten bir konumu gerekir. Yazılım vitesi motor komut yüzdesini değiştirir, PWM frekansını değiştirmez. D bir aç/kapa anahtarı ise geri kapatılana kadar aktif kalır; kod basış başına toggle yapmaz.

ELRS tarafında CH10'u taşıyan bir kanal modu kullan. **Hybrid** CH1–CH12 içindeki bu anahtar düzenini taşıyabilir. Full Resolution **8ch** kullanırsan CH10 aktarılmaz. ELRS/EdgeTX'te “Arm using Switch” ayarı bazı modlarda CH5 mix'ini geçersiz kılabilir; bu nedenle hem TX12 kanal monitörünü hem ESP32 Seri Monitör'deki CH5/CH10 değerlerini kontrol et. E'ye atanan CH5 seçimini alıcıya ulaşan değer belirler.

ESP32 monitöründeki yaklaşık değerler: −100 → 988–1000, orta → 1500, +100 → 2000–2012 µs. C en altta hâlâ 1000 görünüyorsa CH8'i kumandada ters çevir veya `config.h` içindeki `AUTO_INPUT_SIGN` değerini `-1` yap. İkisini birden ters çevirme.

## 3. Modların davranışı

| Mod | Ana motorlar | Pan / tilt | Lazer |
|---|---|---|---|
| MANUAL | CH1/CH2 ve CH6 | Joystick/Jetson hareket komutu almaz; daha önce etkinleşmişse son konumu tutar | Kapalı |
| SERVO_LASER | Her çevrim STOP gönderilir | CH1/CH2 ile hız kontrollü; joystick bırakılınca konumu tutar | D aktifken açık |
| AUTO | Jetson sol/sağ komutları | Jetson pan/tilt hedefleri | Jetson lazer komutu |
| Kumanda bağlantısı yok | STOP gönderilir | Yeni hareket komutu yok, mevcut konum tutma sinyali korunur | Kapalı |

**CH8 AUTO her zaman CH5'ten önceliklidir.** AUTO'da CH1/CH2, F ve D çıkışları yönetmez. C'yi yukarı alırsan E'nin seçtiği moda dönersin. Jetson kumandadaki modu değiştiremez.

Servo beslemesi yazılımla kesilmez; MANUAL'de “çalışmama” hareket komutlarının engellenmesi anlamındadır. Tamamen enerjisiz servo isteniyorsa ayrı bir güç anahtarlama donanımı gerekir.

Açılışta servo PWM darbesi gönderilmez. İlk geçerli servo/otonom oturumunda kayıtlı başlangıç hedefi 1500 µs uygulanır; servo fiziksel olarak orada değilse hareket edebilir. İlk mekanik orta ayarını servo kolu yükten ayrılmışken yap.

## 4. Sinyal bağlantıları

Bağlantıları tüm güçler kapalıyken yap. **GPIO numarası, kartın fiziksel sıra numarası değildir.**

| Kaynak | Hedef |
|---|---|
| ELRS alıcısı TX (CRSF çıkışı) | ESP32 GPIO16 |
| ESP32 GPIO25 | Sol motorları süren Reactor'un **SRL** girişi |
| ESP32 GPIO26 | Sağ motorları süren Reactor'un **SRL** girişi |
| ESP32 GPIO18 | Pan RDS3235 sinyal |
| ESP32 GPIO19 | Tilt RDS3235 sinyal |
| ESP32 GPIO27 | 3,3 V mantıkla uyumlu lazer röle modülü IN |
| BNO055 SDA | ESP32 GPIO32 |
| BNO055 SCL | ESP32 GPIO33 |
| BNO055 VIN | ESP32 **3V3** (5V verme) |
| BNO055 GND | ESP32 GND (ortak referans) |
| USB-TTL TX | ESP32 GPIO21 |
| USB-TTL RX | ESP32 GPIO22 |
| USB-TTL GND | ESP32 GND |
| USB-TTL VCC / 3V3 / 5V | **Bağlanmayacak** |

ELRS alıcısı RX, bu tek yönlü kumanda okumasında kullanılmıyor. Alıcının besleme voltajını modeline göre belirle; TX12 ürün linki alıcı modelini belirtmez. ESP32 girişlerine gelen seri sinyal **3,3 V mantık** olmalı. Dönüştürücü üzerindeki “3,3 V” besleme pini TX mantığının da 3,3 V olduğunu tek başına kanıtlamaz.

İki Reactor'un her birinde A/B motor çıkışları aynı taraftaki ön/arka motorları sürer. Her motorun kablo yönü farklı olabilir. `config.h` içindeki `INV_LEFT_A/B` ve `INV_RIGHT_A/B` bunu ayrı ayrı ters çevirir. Mevcut dosyadaki sol +1, sağ −1 değerleri korunmuştur; bunlar fiziksel yön testi yerine geçmez.

Reactor'ları güç kapalıyken **UART / MODE 3** seç: üretici kılavuzuna göre DIP **1 ve 2 yukarı, 3 aşağı**. Hat 38400 baud 8N1; SRL'ye ham byte gider. A duruşu 64, B duruşu 192'dir. Bu kullanımda RC kalibrasyonu gerekmez. SRL ile sürücünün teşhis TX çıkışını karıştırma.

## 5. Güç, röle ve acil stop

- 24 V batarya, uygun korumalı güç dağıtımından Reactor güç girişlerine gider. **ESP32, alıcı, servo veya lazer GPIO'suna 24 V verilmez.**
- ESP32 ilk testte USB'den beslensin. Servolar kendi modelinin izin verdiği çıkış voltajına ayarlanmış ayrı, yeterli akım kapasiteli regülatörlerden beslensin; ESP32 üzerinden besleme.
- ESP32, alıcı, USB-TTL, Reactor sinyal GND'leri ve servo regülatörü çıkış GND'leri ortak referansa bağlanır. Motor/servo güç dönüş akımını ESP32 kartının veya ince sinyal GND kablosunun üzerinden geçirme; güç dönüşleri güç dağıtımına gitmeli.
- Regülatörlerin pozitif çıkışlarını birbirine bağlama. USB ile başka bir 5 V kaynağını kartın 5 V hattında gelişigüzel paralelleme.
- Röle kontakları: lazerin kendi uygun besleme **+ → COM**, **NO → lazer +**, lazer **− → kendi besleme −**. Röle bobini doğrudan GPIO'ya bağlanmaz. Röle modülünün beslemesi ve giriş mantığı modele bağlıdır.
- `LASER_ACTIVE_HIGH=1` varsayılanı, HIGH ile çeken modül içindir. LOW ile çeken modülde 0 yap. ESP32 reset durumunda röleyi kapalı tutan uygun donanım bias'ı da olmalı. Bunu lazer gücü ayrılmışken röle göstergesi/kontak ölçümüyle kontrol et.

**EN bağlantısı varsayılan kapalıdır (`REACTOR_EN_PIN=-1`). GPIO23'ü doğrudan EN'e bağlama.** Üretici EN için 5 V HIGH tanımlar ve fabrikadan enable bypass köprüsü kapalı gelir. EN ile durdurma eklenirse doğru 3,3→5 V tampon, reset sırasında LOW kalma ve üreticinin bypass düzenlemesi birlikte gerekir. Hazır EN bağlantısı varmış gibi güvenlik varsayımı yapılmamıştır.

**Reactor UART modunda son hızını korur.** Yazılım kumanda/Jetson kesilince stop byte'ları yollar; ESP32 enerjisiz kalırsa veya SRL kablosu koparsa bu byte'lar sürücüye ulaşamaz. Bağımsız fiziksel acil stop devresi bu sebeple gereklidir. Yazılım STOP'u aracın mekanik olarak anında durmuş olduğunu ölçmez.

## 6. Kilitler ve ilk test sırası

1. Önce ana motor, servo ve lazer güçleri ayrıyken ESP32/alıcıyla kanalları doğrula. C alt → AUTO; C diğer + E yüksek → MANUAL; E düşük → SERVO_LASER.
2. İlk güç verili motor testinde tekerlekler yerden kesilmiş ve araç sabitlenmiş olsun. F düşük hızda olsun. Açılıştan sonra 1,2 s bekleme ve joystick ortada 0,5 s koşulu sağlanınca `ARM:1` olur. Her mod geçişi veya kumanda kesintisi bu kilidi sıfırlar. SERVO_LASER'de motor `ARM:0` olması normaldir.
3. Çok küçük ileri komutuyla dört motorun yönünü tek tek doğrula. Yanlış kanalı `INV_*` ayarıyla düzelt; yanlış yöne giden bir motoru yük altında zorlamaya devam etme.
4. Servo mekanik orta ve limitlerini test et. `PAN_MIN/MAX_US`, `TILT_MIN/MAX_US`, `*_CENTER_US`, `*_INPUT_SIGN`, `*_RATE_US_PER_S` config.h içindedir. V2 varsayılan sınırları pan 700–2300, tilt 1050–1900 µs; **mekaniğe uygunluğu doğrulanmış değildir**. Teste merkez çevresindeki dar aralıkla başla. 0–180 yazılım hedefi servo milinin ölçülmüş fiziksel derecesi değildir; RDS3235 varyantına göre fark eder.
5. SERVO_LASER'e girerken joystick ortada 0,5 s tutulur. D önceden basılıysa lazer açılmaz: önce bırakıp tekrar bas. MANUAL'e geçince lazer kapanmalı, joystick servoyu oynatmamalı. SERVO_LASER'de joystick motorları oynatmamalı.
6. Kumandayı kapatınca son geçerli CRSF kanal paketinden 300 ms sonra (kontrol çevrimi gecikmesi eklenir) STOP/lazer OFF uygulanır. Geçerli LQ=0 bildirimi de linki kapatır. Jetson motor komutu 200 ms bayatlarsa AUTO durur; heartbeat tek başına hareketi sürdürmez.
7. Otonoma geçişte Jetson önce 0,5 s sıfır motor komutu ve AUTO_REQ gönderir. Eski moddaki paketler atılır. Kayıp bağlantı geri geldiğinde de sıfır komutla yeniden etkinleşme gerekir.

CRC, sürüm, uzunluk, komut aralığı ve sıra kontrolleri var. Bozuk/tekrar/eski sıra paketleri komut zaman aşımını yenilemez. RMT/LEDC kurulum veya RMT gönderim hatası yazılım hata kilidi oluşturur; fiziksel bağlantı kopmasını ölçen geri bildirim yoktur.

## 7. Jetson / ROS 2

Jetson USB-A → 3,3 V mantıklı USB-TTL → GPIO21/22/GND. ESP32 USB'si programlama ve teşhis içindir. ROS 2 kurulumunun bulunduğu terminali aç; kendi dağıtımına ait setup.bash dosyasını source et. Bu köprü ROS 1 için değildir.

```bash
sudo apt install python3-serial
cd ika_esp32/jetson
python3 ika_ros2_bridge.py --ros-args -p port:=/dev/ttyUSB0
```

`/dev/ttyUSB0` örnektir: kendi dönüştürücünün portunu veya `/dev/serial/by-id/...` yolunu kullan. Erişim hatası varsa kullanıcının seri port izinlerini düzelt. Aynı portu iki programla açma.

| ROS 2 topic | Tip | Anlam |
|---|---|---|
| `/cmd_vel` | geometry_msgs/msg/Twist | linear.x ileri, angular.z sola dönüş; en az 10–20 Hz yayımla |
| `/ika/pan` | std_msgs/msg/Float32 | 0–180 mantıksal konum; son hedef tutulur |
| `/ika/tilt` | std_msgs/msg/Float32 | 0–180 mantıksal konum; son hedef tutulur |
| `/ika/laser` | std_msgs/msg/Bool | True yalnız taze kaldığı sürece açık; açık tutmak için en az 10–20 Hz yayımla |
| `/ika/status` | std_msgs/msg/String | JSON mod/link/armed/çıkış komutları/hata bilgisi |
| `/ika/imu` | std_msgs/msg/String | JSON pitch/yaw (derece) + BNO055 kalibrasyon durumu; ~20 Hz (sadece okuma) |

Köprü 50 Hz seri komut üretir. Varsayılan `drive_limit_pct=30`; bu ilk test sınırıdır. F yalnız manuel modu etkiler. `max_linear_mps=1.0`, `max_angular_radps=1.0` komut ölçekleridir: enkoder/kalibrasyon olmadan gerçek hız ölçümü veya 1 m/s garantisi değildir. Projedeki araç hızlarına göre ayarla. Örnek farklı topic:

```bash
python3 ika_ros2_bridge.py --ros-args -p port:=/dev/ttyUSB0 -p cmd_vel_topic:=/robot/cmd_vel -p drive_limit_pct:=30
```

C altındayken köprü sıfır komut göndererek motor kilidinin açılmasını bekler. `/ika/status` içindeki `armed:true` sonrası taze `/cmd_vel` komutları uygulanır. Mod değişiminde önceki hız/lazer isteği temizlenir. `/cmd_vel` veya lazer topic'i 200 ms yenilenmezse ilgili çıktı sıfırlanır. ESP32 status'u 300 ms gelmezse AUTO_REQ kaldırılır. Seri bağlantı yenilenirse önceki hareket talepleri temizlenir.

Jetson'daki mevcut algılama/rota düğümleri pakete dahil değildir. Köprü `/cmd_vel` ve yukarıdaki topic'lerden komut bekler; C'yi indirmek tek başına parkur algoritması oluşturmaz. ROS düğümleriniz farklı mesaj tipleri/protokoller kullanıyorsa bu arayüze bağlanmalıdır.

Windows'ta yalnız iki yönlü olmayan telemetri okuma kontrolü (motor komutu göndermez):

```powershell
py -m pip install pyserial
cd jetson
py serial_monitor.py COM22
```

COM22 örnektir; USB-TTL'nin güncel COM numarasını kullan. ESP32'nin kendi USB Seri Monitörü ayrı porttur.

## 8. Seri protokol ve doğrulama

`AA 55 | VERSION=01 | TYPE | LENGTH | SEQ | PAYLOAD | CRC_L CRC_H`

CRC-16/CCITT-FALSE, init FFFF, polynomial 1021; VERSION'dan payload sonuna kadar hesaplanır. SEQ tek byte, komut ve heartbeat dahil aynı gönderici sayacıyla her çerçevede artar ve 255→0 döner.

COMMAND type 02, payload 7 byte: `int8 sol(-100..100), int8 sağ, uint8 pan(0..180), uint8 tilt, uint8 lazer(0/1), uint8 mod_rezerve(FF), uint8 flags(bit0=AUTO_REQ)`.

HEARTBEAT type 03: kaynak (Jetson=1, kart=0) + uint32 uptime_ms little endian. Taze COMMAND link zamanını da yeniler. Ayrı heartbeat hareket yetkisi vermez.

STATUS type 01, payload 8 byte: sol komut %, sağ komut %, pan, tilt, lazer, mod (0 manuel/1 servo/2 auto), CRSF link, flags. Flags: 01 Jetson link, 02 komut timeout, 04 auto hazır, 08 failsafe, 10 CRC hatası, **20 motor armed**, **40 donanım hata kilidi** (hex). Bunlar ölçülmüş motor hızı/servo pozisyonu değil, uygulanan komutlardır. 04 tek başına motor kilidinin açıldığını göstermez; köprü 20 bitini kullanır.

IMU type 04, payload 5 byte (BNO055, karttan Jetson'a, ~20 Hz): `int16 pitch(derece×100, LE), uint16 yaw(derece×100, 0..35999, LE), uint8 cal`. `cal` bayti: bit[7:6]=sistem, [5:4]=jiroskop, [3:2]=ivme, [1:0]=manyetometre (her biri 0-3; 3=tam kalibre). **Sadece telemetridir; motor/servo/lazer kontrolünü ve `g_hw_fault` kilidini etkilemez.** IMU yoksa/bozuksa bu çerçeve gönderilmez, sürüş normal çalışır. Köprü bu çerçeveyi `/ika/imu` ROS 2 topic'ine (JSON String) yayınlar.

Test sonucu: ESP32 Arduino core **3.3.0**, **esp32:esp32:esp32** ile gerçek derleme başarılı; flash 330359 byte, statik RAM 22004 byte. V2 gerçek controller.cpp üzerinde host testleri mod önceliğini, joystick merkez kilidini, D bırakma koşulunu, timeout'ları, bozuk/tekrar paketleri, RMT UART bitlerini ve hata kilidini geçti. V2 ek testleri eşit büyüklükte motor komutlarını, yön değiştirmede sıfır beklemesini, dönüş hız sınırını, zaman sayacı taşmasını, servo merkez filtresini ve 1050 µs üst sınırını geçti. Python protokol/köprü tarafında 10 test geçti. ROS 2'nin gerçek Jetson çalışma ortamı, osiloskop sinyali ve fiziksel araç testi yapılmadı.

Tekrar çalıştırma (g++ ve Python bulunan ortam):

```bash
g++ -std=c++17 -I tests/stubs tests/firmware_test.cpp -o /tmp/ika_firmware_test
/tmp/ika_firmware_test
g++ -std=c++17 -I tests/stubs tests/firmware_v2_test.cpp -o /tmp/ika_firmware_v2_test
/tmp/ika_firmware_v2_test
python3 -m unittest discover -s tests -p 'test_*.py'
```

## Kaynaklar

- [JSumo Reactor V1.2 üretici kılavuzu](https://blog.jsumo.com/wp-content/uploads/2025/10/Reactor-Manual-Datasheet.pdf): UART, pinler ve EN; s. 1, 5, 7.
- [JSumo ürün sayfası](https://www.jsumo.com/reactor-v12-dual-dc-brushed-motor-driver-60v-100a-x-2-output): UART girişinin 3,3 V MCU uyumluluğu.
- [Espressif RMT API](https://docs.espressif.com/projects/arduino-esp32/en/latest/api/rmt.html), [LEDC API](https://docs.espressif.com/projects/arduino-esp32/en/latest/api/ledc.html), [Arduino kurulumu](https://docs.espressif.com/projects/arduino-esp32/en/latest/installing.html).
- [ExpressLRS kanal modları](https://www.expresslrs.org/software/switch-config/), [Arm using davranışı](https://www.expresslrs.org/quick-start/transmitters/tx-prep/).
- [Pololu: servo kontrol sinyali](https://www.pololu.com/blog/17/servo-control-interface-in-detail), [Pololu teknik destek: mekanik yük altında titreme](https://forum.pololu.com/t/micro-maestro-and-jitter/4975).
- [TBS CRSF protokolü](https://github.com/tbs-fpv/tbs-crsf-spec/blob/main/crsf.md).

Hazırlanma: 6 Eylül 2026. Mevcut `argex_esp32(2).ino` dosyasındaki pinler ve çerçeve düzeni temel alınmıştır; eski dosya değiştirilmemiştir.


## Bu paket icin kontrol notu

- Manuel surus, titreme-fix oncesindeki klasik diferansiyel mikse geri alinmistir: `sol=gaz+donus`, `sag=gaz-donus` (TURN_GAIN uygulanir).
- Donuslerde iki tarafin hizini zorla esit tutan pivot/rampa profili kullanilmaz.
- Servo titreme filtresi ve artirilmis tilt ust hareketi korunur.
- OTONOM moddaki ikinci 500 ms / sifir-komut arm beklemesi kaldirilmistir; Jetson heartbeat/komut timeout ve diger failsafe'ler korunur.
