# SmartBus Master No-TFT HotspotRecovery v1.9 (4 MB)

Branch-folder ini adalah salinan `Code_Master` untuk ESP32 Master tanpa aset
atau kode TFT.

## Konfigurasi

- Board: DOIT ESP32 DevKit V1
- Flash fisik: 4 MB
- Partisi: `default.csv` (mendukung OTA)
- Framework: Arduino ESP32 standar melalui `espressif32@7.0.1`
- Flasher: `esptool.py 4.11.0`, baud 115.200
- Audio DY-HV8F dan WebSocket head unit tetap aktif
- NAPT/internet sharing tidak disertakan dalam branch standar ini
- Roster: 64 slot untuk target produksi 50 seat dan 14 slot cadangan
- Pending pairing dan antrean penerimaan panggilan: 64 entri

Seat produksi memakai UDP bersama pada port 5758, sehingga batas 15 client
WebSocket bukan batas jumlah seat. Slot WebSocket dipakai head unit dan hanya
relevan untuk gateway/seat legacy yang masih memakai WebSocket.

## Audio DY-HV8F (DIP 000)

Firmware menggunakan integrated I/O mode dengan input aktif-LOW. Hubungkan:

| DY-HV8F | ESP32 |
| --- | --- |
| IO0 | GPIO13 |
| IO1 | GPIO14 |
| IO2 | GPIO19 |
| IO3 | GPIO21 |
| IO4 | GPIO22 |
| IO5 | GPIO23 |
| IO6 | GPIO25 |
| IO7 | GPIO26 |
| BUSY | GPIO15 |
| GND | GND |

Atur DIP ke `000` sebelum menyalakan modul. Semua input HIGH berarti idle;
firmware memberikan pulsa kombinasi biner LOW selama 150 ms untuk memilih
track 1 sampai 255. GPIO16 dan GPIO17 tidak lagi digunakan untuk audio.

Firmware memakai 20 track dari
`audio/track baru sudah di convert/TRACK_MAPPING.md`. Saat tombol panggil
ditekan, ID kursi dibaca otomatis dan pengumuman diputar dua kali. Format ID
produksi adalah `A`, `B`, nomor `1` sampai `99`, atau nomor dengan akhiran A/B
seperti `21A`. Nomor dirakit dari track angka, `belas`, dan `puluh`; huruf A/B
memakai track 15/16. Format lain ditolak saat pairing agar tidak ada ID tanpa
pengumuman audio.

### Peringatan seatbelt (track 17-20)

Seat melaporkan status buckle pada field `"seatbelt"` di setiap paket UDP.
Master memutar peringatan **satu kali** saat sebuah kursi terisi tetapi
seatbelt belum terpasang (rising edge), dengan kalimat:

```text
kursi nomor <N> tidak memakai seatbelt
```

Track yang dipakai: `20` ("kursi nomor") + track angka yang sudah ada +
`17` ("Tidak") + `18` ("Memakai") + `19` ("Seatbelt"). Track 1-16 tidak
berubah. Salin file MP3 baru ke modul DY-HV8F pada urutan track 17, 18, 19,
20 (penomoran bersifat posisional, jangan mengubah urutan track lama).

Status seatbelt juga tampil di roster JSON (`seatbelt`, `seatbeltAlert`), di
tabel `seats` pada serial, dan di kolom **Seatbelt** web monitor.

## Perbaikan LED v1.6

- Pengumuman panggilan diputar oleh task audio terpisah, sehingga animasi LED
  tetap berjalan selama audio berlangsung.
- Dimmer 0% dan perintah OFF mengirim data hitam eksplisit agar LED benar-benar
  mati.
- Urutan warna strip dikoreksi dari `RBG` menjadi `GRB`, sehingga kontrol RGB
  manual kembali sesuai merah, hijau, dan biru.

## Dynamic sensor ID v1.7

Sensor tanpa ID tersimpan melapor sebagai `RandomID` dengan UID hardware unik.
Tekanan tombol panggilan pertamanya mengirim event `seatPairingRequired` hanya
ke head unit. ID yang dipilih aplikasi diteruskan kembali ke sensor yang cocok,
termasuk bila sensor berada di balik rangkaian Gateway. Sensor menyimpan ID ke
NVS dan mengirim konfirmasi `seatIdSaved`; `RandomID` tidak dimasukkan roster
dan tidak memicu audio sebelum provisioning selesai.

## Router-centric STA-only

Master, seluruh seat, dan Head Unit terhubung langsung ke router `iot_bus`.
Master tidak lagi memancarkan `Seat-Monitoring-Bus` dan Gateway tidak diperlukan.
Seat mengirim data melalui satu listener UDP Master pada port 5758, sedangkan
Head Unit mempertahankan satu koneksi WebSocket ke Master pada port 81.

Master dapat ditemukan melalui `bus-led.local`, IP terakhir, atau scan subnet
router. Sebaiknya buat DHCP reservation untuk Master supaya alamat web monitor
dan Head Unit selalu tetap. AP/client isolation router harus dimatikan.

### Kredensial bawaan

Router armada dikunci pada nilai berikut:

```cpp
const char* STA_SSID = "iot_bus";
const char* STA_PASS = "iot_BUS@5758";
```

Kredensial hotspot lama yang tersimpan di NVS diabaikan. Gunakan `status` atau
`wifi` untuk melihat SSID dan IP LAN yang diberikan router.

## Pemulihan router

Master memakai auto-reconnect STA dan mencoba kembali setiap lima detik tanpa
restart. Seat juga reconnect dan menemukan ulang Master melalui UDP broadcast.
Setelah koneksi Master pulih, listener UDP di-bind ulang dan alamat web monitor
dicetak kembali ke serial.

Indikator LED built-in menyala solid ketika router terhubung dan mati ketika
koneksi router belum tersedia.

## Build dan upload

```powershell
pio run -e master_4mb
pio run -e master_4mb -t upload --upload-port COM9
```

Upload PlatformIO pada branch ini memakai perintah standar untuk board Master
4 MB yang terdeteksi dengan JEDEC flash ID `5e:4016`.
