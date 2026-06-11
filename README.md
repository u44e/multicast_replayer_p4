> [!IMPORTANT]
> **このリポジトリはモノレポ [multicast_replayer_zero_pi](https://github.com/u44e/multicast_replayer_zero_pi) に統合され、アーカイブされました (2026-06-11)。**
> P4コンソール版は `apps/p4_console` に移植済み (mcreplay API)。今後の開発・修正は統合先で行います。

# Multicast Replayer for ESP32-P4 (有線Ethernet)

ESP32-P4 + RMII PHY (100Mbps Ethernet) 向けのマルチキャストUDPストリーム
リプレイヤー。microSD (SDMMC 4-bit) 上のPCAPファイルからマルチキャスト
ストリームを抽出し、**元のキャプチャタイムスタンプ間隔を再現しながら**
有線LANへ再送信します。

Cardputer-Adv版 ([multicast_replayer_adv_esp32](https://github.com/u44e/multicast_replayer_adv_esp32))
の有線・大容量版です。P4はWi-Fi非搭載のため有線専用。
8Mbps級の映像ストリームを複数本同時に流す用途を想定しています。

## ハードウェア

- **SoC**: ESP32-P4 (PSRAM付きモジュール推奨。インデックスをPSRAMに配置)
- **Ethernet**: 内蔵EMAC + RMII PHY (IP101/LAN87xx/RTL8201/DP83848/KSZ80xx を menuconfig で選択)
- **microSD**: SDMMC 4-bit
- ピンはIDFのP4既定値 (ESP32-P4-Function-EV-Board 相当):
  - RMII: MDC=31, MDIO=27, REF_CLK_IN=50, TX_EN=49, TXD0=34, TXD1=35, CRS_DV=28, RXD0=29, RXD1=30
  - SD: CMD=44, CLK=43, D0-D3=39-42, 電源=内蔵LDOチャネル4
  - 変更は `idf.py menuconfig` → "Multicast Replayer (P4)"

## ビルド＆フラッシュ

```bash
source $IDF_PATH/export.sh        # ESP-IDF v5.3 以上
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/tty.usbmodem* flash monitor
```

## 操作 (USB-Serial-JTAG コンソール)

```
mcr> load /sdcard/capture.pcap    # PCAP読み込み＆インデックス化
mcr> list                         # ストリーム一覧 (送信統計含む)
mcr> start all                    # 全ストリーム再生 / start 0 で個別
mcr> stop                         # 全停止
mcr> opts -p 6000 -s 0            # ポート上書き / seq埋め込み(0|1)
mcr> stat                         # リンク状態・ヒープ・送信pps
```

ネットワークはDHCPで自動取得 (`stat` でIP確認)。

## 設計メモ

- ロード時にPCAPを2パス走査し、ストリームごとに各パケットの
  「ファイルオフセット/長さ/相対時刻」をインデックス化 (10B/パケット)。
  上限20万パケット (~2MB、`CONFIG_SPIRAM_USE_MALLOC` でPSRAMに配置)。
  8Mbps映像 (~760pps) なら約4分強/本に相当
- 再生は専用FreeRTOSタスク (Core 1, 優先度10) が全ストリームの次パケット
  送出予定時刻を比較して最早のものまでスリープ。1ms未満はビジーウェイト
  (`CONFIG_FREERTOS_HZ=1000`)
- 送信はUDPペイロードを取り出して通常ソケットで送信、`IP_MULTICAST_TTL=64`
- `opts -s 1` でペイロード先頭に2バイトseqを付加可能
  (**ペイロードが変わるため実映像ストリームではoffのまま**)

## 性能の目安

- SDMMC 4-bit: 実効 10-20MB/s → SD読み出しはボトルネックにならない
- 100M Ethernet: UDP実効 70-90Mbps → **8Mbps映像 8〜10本**
- ストリーム上限: `MAX_STREAMS` = 32

## 実機検証が必要な箇所

- [ ] RMII PHYの動作 (PHYモデル・アドレス・リセットGPIOは menuconfig)
- [ ] SDスロットのLDO電源 (EV board以外は `MCR_SD_LDO_CHAN=-1` で外部電源)

## テスト用PCAP

```bash
python3 gen_test_pcap.py   # 10ストリーム x 3パケット (100ms間隔) を生成
```
