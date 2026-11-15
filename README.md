# wifi_pwr_ctrl

Linux向けのC言語製CLIです。ローカルネットワーク上のTP-Link Tapo P110M / P105 / P300とKasa HS105をIPv4アドレスで指定して、電源の操作と状態確認を行います。P110Mでは消費電力・電力量も取得できます。

| 機種 | 電源ON/OFF・状態確認 | 消費電力・電力量 |
|---|---|---|
| Tapo P110M | 対応 | 対応 |
| Tapo P105 | 対応 | 非対応 |
| Tapo P300 | コンセントごとに対応 | 非対応 |
| Kasa HS105 | 対応 | 非対応 |

TapoはKLAP v2のローカルプロトコルを使用します。AES方式など、KLAP v2に対応しないファームウェアはサポートしていません。P300のUSBポートは機器が公開する子デバイスとして表示される場合に限り操作できます。機器やファームウェアにより利用できる機能が異なる場合があります。

## ビルド

Cコンパイラ、`make`、`pkg-config`、OpenSSL、libcurl、json-cの開発パッケージが必要です。Debian / Ubuntuでの例:

```sh
sudo apt install build-essential pkg-config libssl-dev libcurl4-openssl-dev libjson-c-dev
make
```

## 使い方

Tapo機器は、事前にTapoアプリでセットアップしておきます。TP-Linkアカウントのメールアドレスとパスワードを環境変数で指定してください。HS105には認証情報は不要です。

```sh
export TAPO_USERNAME='メールアドレス'
read -rs -p 'TP-Link password: ' TAPO_PASSWORD; echo
export TAPO_PASSWORD

./wifi-pwr-ctrl --model P110M --ip 192.168.1.10 status
./wifi-pwr-ctrl --model P110M --ip 192.168.1.10 on
./wifi-pwr-ctrl --model P110M --ip 192.168.1.10 off
./wifi-pwr-ctrl --model P110M --ip 192.168.1.10 energy

./wifi-pwr-ctrl --model P105 --ip 192.168.1.11 status
./wifi-pwr-ctrl --model HS105 --ip 192.168.1.12 on
```

P300ではまず`status`で子デバイスのIDを確認し、操作するIDを`--child`で指定します。位置番号は機器から返された場合に表示されます。

```sh
./wifi-pwr-ctrl --model P300 --ip 192.168.1.13 status
./wifi-pwr-ctrl --model P300 --ip 192.168.1.13 --child '表示されたdevice_id' on
./wifi-pwr-ctrl --model P300 --ip 192.168.1.13 --child '表示されたdevice_id' off
./wifi-pwr-ctrl --model P300 --ip 192.168.1.13 --child '表示されたdevice_id' status
```

出力はJSONです。`status`の`device_on`は電源状態、`energy`の`power_w`は現在の消費電力（W）、`today_kwh`と`month_kwh`は機器の今日・今月の積算電力量（kWh）です。`energy`は1回の測定値を取得します。非対応機器での実行はエラーになります。操作成功時は`{"ok":true}`を出力します。

デフォルトのポートはTapoがTCP 80、HS105がTCP 9999です。必要に応じて`--port`で変更できます。終了コードは成功時0、通信・認証・機器エラー時1、引数・非対応機能エラー時2です。認証情報はコマンド引数や履歴に入力せず、共有環境では環境変数の公開範囲にも注意してください。

## テスト

`make test`でローカルの模擬機器を使ったテストを実行します。Python 3と`cryptography`パッケージが必要です。実機による動作確認の代わりにはなりません。
