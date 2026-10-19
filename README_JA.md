# ZMK LED Patterns

[![Test](https://github.com/amgskobo/zmk-led-patterns/actions/workflows/test.yml/badge.svg)](https://github.com/amgskobo/zmk-led-patterns/actions/workflows/test.yml)

[English](README.md)

外付け LED 1 つ向けの 18 種類のアニメーションパターンを、ZMK の behavior として
提供します。split キーボードでは接続インジケーターも兼ね、LED は何よりも先に、
左右が互いを見つけたかどうかを表示します。

- 設定ごとに 1 つの keymap behavior(`&led_pattern`、`&led_brightness`、
  `&led_speed`)。それぞれパラメーターの metadata を持つため、Studio の keymap
  editor で割り当てられます。
- 任意で LED の状態全体を `zmk-feature-custom-settings` を通じて公開し、client が
  専用の画面なしで編集できるようにします。BLE のキー操作は設定に即時反映し、
  USB の連続操作は設定通知をまとめます。split の表示変更は共通の将来時刻に適用します。
- 固定周期のアニメーション tick はありません。各パターンが次の変化時刻を返します。
  idle-off 有効時は idle 中の通常アニメーションを止めます。設定・接続・電源の
  有効な変化には、それぞれ上限のある処理が残ります。

## インストール

### 1. module を追加する

利用する config の `config/west.yml` に追加します。

```yaml
  remotes:
    - name: amgskobo
      url-base: https://github.com/amgskobo
  projects:
    - name: zmk-led-patterns
      remote: amgskobo
      revision: main
```

### 2. LED を指定する

この module は `zmk,backlight` chosen node の子 index 0 を駆動するため、board または
shield でそこに PWM 駆動の LED を置く必要があります。

```dts
/ {
    chosen {
        zmk,backlight = &backlight;
    };

    backlight: pwmleds {
        compatible = "pwm-leds";

        pwm_led_0 {
            pwms = <&pwm0 0 PWM_MSEC(10) PWM_POLARITY_NORMAL>;
        };
    };
};
```

あわせて PWM driver(`CONFIG_PWM=y`)が必要です。`pwm-leds` node があれば Zephyr が
`LED_PWM` を自動で有効にし、`LED` は module 自身が select します。chosen node は
LED を見つけるためだけに使うので、ZMK の backlight subsystem は不要で、無効のままに
しておくのが最適です。`CONFIG_ZMK_BACKLIGHT=y` にすると、`backlight.c` も起動時に
その LED へ書き込み、`&bl` が 2 つ目の所有者になります。

### 3. behavior を宣言する

```dts
#include <dt-bindings/zmk/led_pattern.h>

/ {
    behaviors {
        led_pattern: led_pattern {
            compatible = "zmk,behavior-led-pattern";
            #binding-cells = <1>;
        };

        led_brightness: led_brightness {
            compatible = "zmk,behavior-led-brightness";
            #binding-cells = <1>;
        };

        led_speed: led_speed {
            compatible = "zmk,behavior-led-speed";
            #binding-cells = <1>;
        };
    };
};
```

`led_pattern` node は必須です。他の 2 つが操作する LED controller を所有しており、
これなしで他の node を宣言するとメッセージ付きでビルドが失敗します。brightness と
speed の node は任意です。node 名は自由に付けられます。設定キーは node 名から
作らないため、後から名前を変えても保存済みの値は失われません。各 compatible の
node は 1 つまでにしてください。重複は、最初の 1 つだけを黙って登録するのではなく、
ビルド時に拒否します。

### オプション

| symbol | default | |
| --- | --- | --- |
| `CONFIG_ZMK_LED_PATTERNS` | `zmk,behavior-led-pattern` node があると有効 | behavior とアニメーション。`LED` を select します。 |
| `CONFIG_ZMK_LED_PATTERNS_CUSTOM_SETTINGS` | `n` | client が編集できるよう状態を公開し、キー押下もその設定に書き込みます。`ZMK_CUSTOM_SETTINGS_STUDIO_RPC` が必要です。 |

module は `LED` と `ZMK_LOW_PRIORITY_WORK_QUEUE` を select し、split ビルドでは
`ZMK_SPLIT_RELAY_EVENT` も select します。

## behavior

設定ごとに 1 つの behavior があり、それぞれパラメーターを 1 つ取ります。

| behavior | parameter | |
| --- | --- | --- |
| `&led_pattern` | 0 - 17 | そのパターンを選択 |
| | `LED_PATTERN_PREVIOUS`, `LED_PATTERN_NEXT` | 前または次のパターンへ。端では反対側へ回ります |
| `&led_brightness` | 0 - 100 | その明るさ(%)に設定 |
| | `LED_BRIGHTNESS_DOWN`, `LED_BRIGHTNESS_UP` | 0 から 100 の範囲で 10 ポイント下げる/上げる |
| `&led_speed` | 10 - 400 | その速度(%)に設定 |
| | `LED_SPEED_DOWN`, `LED_SPEED_UP` | 10 から 400 の範囲で 10 ポイント下げる/上げる |
| | `LED_SPEED_MIN`, `LED_SPEED_DEFAULT`, `LED_SPEED_MAX` | 10、100、400 % |

```dts
&led_pattern LED_PATTERN_HEARTBEAT
&led_pattern LED_PATTERN_NEXT
&led_brightness LED_BRIGHTNESS_DOWN
&led_brightness 40
&led_speed 200
&led_speed LED_SPEED_UP
&led_speed LED_SPEED_DEFAULT
```

段階的な変更は 10 の倍数に揃います。client から明るさを 95 にした場合、端数の 5 を
残さず 100 か 90 へ移ります。速度の段階変更も上下限の間で同じように動きます。

明るさ 0 は LED driver に 0 を要求し、明るさが再び上がるまで通常アニメーションの
再描画を止めます。実際の電気的な出力や電流は driver と PWM 回路に依存します。
split も定期的な時計通信は行わず、設定や接続などの有効な変化があった場合だけ
必要な同期を行います。再び明るくするとき、再利用可能な新しい時計なら位相を維持
できます。古い時計や refit が必要な場合は、検証した将来の共通時刻から新しい位相を
開始します。明るさ 0 でも点灯するのは split の接続インジケーターだけです。これは
診断用であり、LED の設定に関係なく同じ見た目である必要があるからです。advertising
インジケーターはパターンと同じく明るさに応じて変わるため、0 では消灯します。

電源オフのときも LED を消灯します。ZMK の `&soft_off` とアイドルスリープはイベントを
出さず、SoC が止まっている間もピンはそのレベルを保つため、その瞬間に点いていた LED は
ソフトオフ中も点いたままでした(トランジスタ経由なら、解放されたピンが残したゲートの
電荷で同じことが起きます)。`CONFIG_PM_DEVICE`(soft off が選択します)があると、
このモジュールは小さな電源管理デバイスを 2 つ登録します。2 つの電源オフ経路はデバイスを
逆の順序で suspend するため、1 つは init 順で PWM ドライバーより前、もう 1 つは LED
デバイスより後に置きます。先に動いた方がsplit sessionをfenceし、アニメーションを止め、
LED driverが動いているうちに0を書き込んでから送信処理の終了を待ちます。もう一方は
それが済んでいるのを確認するだけです。電源オフが中止されてresumeした場合、splitは
新しい時刻交換と将来予約を開始し、standaloneはlocalの曲線を再描画します。
優先度は `CONFIG_ZMK_LED_PATTERNS_PM_INIT_PRIORITY`(45)と
`CONFIG_ZMK_LED_PATTERNS_PM_LATE_INIT_PRIORITY`(95)です。

速度は、各パターンが描かれた速さに対する百分率です。パターンごとではなく
アニメーションの時計に適用するため、1 つの設定で 18 種類すべてが同じ倍率で速くなり、
特定のパターンだけおかしくなることはありません。

`CONFIG_ZMK_LED_PATTERNS_CUSTOM_SETTINGS=y` の場合、binding は LED を直接変更しません。
現在接続中の transport について behavior の名前に対応する設定へ書き込み、LED はその
設定に従います([Studio 設定](#studio-設定)を参照)。この option がなければ、binding は
controller へ直接要求します。どちらの経路も、standalone は直ちに表示へ反映し、
split は共通の将来の予約時刻に反映します。

各 behavior は ZMK のパラメーター metadata を公開します。これにより Studio の keymap
editor で選択できます。metadata を公開しない behavior はパラメーターを取らないものと
して扱われ、パラメーターが 0 以外の binding はすべて無効として拒否されます。

### 18 種類のパターン

| | constant | | constant |
| --- | --- | --- | --- |
| 0 | `LED_PATTERN_STEADY` | 9 | `LED_PATTERN_BEACON` |
| 1 | `LED_PATTERN_BREATHE` | 10 | `LED_PATTERN_SLOW_BREATHE` |
| 2 | `LED_PATTERN_HEARTBEAT` | 11 | `LED_PATTERN_STROBE` |
| 3 | `LED_PATTERN_BLINK` | 12 | `LED_PATTERN_DOUBLE_BEACON` |
| 4 | `LED_PATTERN_FAST_BLINK` | 13 | `LED_PATTERN_LONG_FLASH` |
| 5 | `LED_PATTERN_TRIPLE_FLASH` | 14 | `LED_PATTERN_SPARKLE` |
| 6 | `LED_PATTERN_SOS` | 15 | `LED_PATTERN_COUNTDOWN` |
| 7 | `LED_PATTERN_CANDLE` | 16 | `LED_PATTERN_RIPPLE` |
| 8 | `LED_PATTERN_SAWTOOTH` | 17 | `LED_PATTERN_FADE_BLINK` |

`LED_PATTERN_COUNT` は最後の番号の次の値です。値を増やすと、切り替えコマンドと設定の範囲も
追従します。パターンの追加時は、曲線と split protocol の範囲を更新し、必要に応じて
左右共通の wire schema / version も更新してください。桁あふれを避ける位相計算に使う
周期の最小公倍数を再計算し、曲線・runtime・codec・64bit 位相のテストも更新します。
スケジュールや wire の契約を変える場合は kernel の確認も行ってください。

## Studio 設定

`CONFIG_ZMK_LED_PATTERNS_CUSTOM_SETTINGS=y` にすると、`amgskobo__led` subsystem の下に
固定の `led.` prefix 付きで 9 つの値を登録します。

| key | type | range | default |
| --- | --- | --- | --- |
| `led.usb_pattern` | int32 | 0 - 17 | 1、breathe |
| `led.usb_brightness` | int32 | 0 - 100 % | 100 |
| `led.usb_speed` | int32 | 10 - 400 % | 100 |
| `led.usb_idle_off` | bool | キーボードが idle になったら消灯 | true |
| `led.ble_pattern` | int32 | 0 - 17 | 1、breathe |
| `led.ble_brightness` | int32 | 0 - 100 % | 100 |
| `led.ble_speed` | int32 | 10 - 400 % | 100 |
| `led.ble_idle_off` | bool | キーボードが idle になったら消灯 | true |
| `led.adv_blink` | bool | advertising インジケーターを表示 | true |

client は専用の画面なしでこれらを表示します。宣言した型と制約が widget を決めます。

**transport ごとに 1 組の設定があります。** 使用中の組は `zmk_endpoint_changed` に
従うため、USB を抜くと点灯も変わります。USB endpoint 以外(BLE profile、または
advertising 中で endpoint がない状態)は BLE の組を読みます。明るさと速度も、パターンと
同じ理由で transport ごとです。USB 接続中のキーボードは明るい部屋の机の上にあり、
同じキーボードでも BLE では暗い場所にあることが多く、「点いている」と感じる数値が
異なるからです。advertising インジケーターだけは分けていません。どの接続かではなく、
接続がまったくないことを示すものだからです。

`idle_off` を transport ごとに持つため、USB 給電中はアニメーションを続け、
BLE 使用中は idle で通常表示を止める方針を選べます。明るさ、パターン、LED 回路、
無線通信も消費電力に影響します。この module では電流や電池の持続時間を実測していません。

パターンが名前付きの dropdown ではなく 0 - 17 の数値なのは、Studio RPC の schema が
options 制約を 8 個までに制限し、handler が何も言わずに切り詰めるからです。18 個の
リストは、最初の 8 個だけが表示され残りの 10 個が失われた状態で client に届きます。
完全な名前は、収まる場所(keymap editor が表示するパラメーター metadata と公開
ヘッダー)にあります。

**キー操作は BLE / USB とも、使用中の設定と desired getter へ即時に反映します。**
standalone は controller へ直ちに反映し、split の表示は左右共通の将来の予約時刻に
切り替えます。予約中の連続操作も、次のキー処理では最新の desired 値を使います。
USB は最後の操作から 300 ms 後に Studio 通知の送出要求をまとめます。
通知時には設定の現在値を読み、古い保留値で後からの client 編集を上書きしません。
flash への保存は最後のメモリー書き込みから 3 秒後に、registry の現在値を
一体として保存します。client からの編集も desired 値へ即時に反映します。メモリー書き込みが
失敗した場合は controller への要求に戻り、standalone / split の表示方針に従います。
その失敗値の registry 保留書き込みや保存予約は残しません。

USB キー操作は Studio 通知だけを遅らせ、core の observer には直ちに通知します。
保存時は USB の Studio CDC ポートが閉じている間だけ Studio 通知を抑えます。
現行 custom-settings は呼び出し単位の quiet event を使い、他の同時書き込みには
通知を残します。互換性を維持する旧 c6a7 依存先は従来の global suppression を
使うため、同時通知を抑制し得る既存の制約が残ります。再接続した client は現在値を
読み取れます。event dispatch の成功は Studio キューや USB / BLE の配送確認ではありません。

これらを登録するのは central だけです。peripheral には独自の設定は不要です。
central に届いたもの(キー押下または client での編集)は、最終状態として peripheral
へ mirror されます。

option が無効の場合(既定であり、upstream ZMK でのビルドもこの状態です)、この部分は
何もコンパイルされず、keymap の binding だけが操作手段になります。

## インジケーター

選択中のパターンより優先して表示されるものが 2 つあります。どちらも装飾より緊急だから
です。split インジケーターは advertising インジケーターより優先されます。

### split の接続

central を時刻の基準とし、LED 専用の論理時計を換算します。Zephyr 全体の時計は変更しません。
左右は共通の将来時刻を予約し、同じ64bit epochから位相を計算します。packetを受信した瞬間には
点灯状態を変更しません。設定値とgetterは操作直後に更新するため、連打や長押しの次の操作も
最新の要求値から進みます。予約済み1件を固定し、その後の変更は最新値1件へまとめます。

| 段階 | LED | 意味 |
| --- | --- | --- |
| WAITING | 固定周期の点滅 | peer未接続 |
| LINKED | 常時点灯 | 有効な点灯予約がまだ成立していない |
| RUNNING_UNCONFIRMED | 予約した通常パターン | 実行中だが完了確認が未確定 |
| SYNCED | 予約した通常パターン | 必要な完了確認があり、現在の時計精度も条件内 |
| COASTING | 最後に成功した表示を自走 | 完了確認済みだが、現在の位相精度は保証範囲外 |
| COASTING_UNCONFIRMED | 最後に成功した表示を自走 | 位相精度と必要な完了確認が未確定 |

接続5秒後から、version4の `ls4` による4メッセージの時刻交換を始めます。両側のboot/link識別子、
sequence、予約状態全体を照合し、古い応答で新しい予約を確認したことにはしません。RTT上限80ms、
モデル上の誤差上限50ms、時刻情報の有効期間8秒を設け、将来の適用時刻と実際の完了時点で検査します。
予約は1000+2RTTms先です。これらはsoftware上の受理条件で、実機で測った左右の発光誤差ではありません。

時計は起動・再接続・休止復帰と、設定や必要なactivity/advertising表示の変更時だけ合わせます。
有効な確認済み時計は将来の適用時刻まで条件を満たす場合だけ再利用し、定期更新はしません。
1回の処理は時刻probeを合計5回、将来予約を3件までとし、各予約の送信は最大3回です。
処理中の連続編集で予算を補充せず、失敗後も同じ要求のまま無限に再送しません。
新しい最新要求は、前の処理の終了時に1回の有限な処理へ引き継げます。

時計精度の期限が切れても、最後に成功した表示はCOASTINGとして自走します。長い放置中の
左右の位相ずれは許容し、次の変更で時計が古ければ、共通の将来時刻から位相を再開します。
新しい出力に失敗した場合は直前に成功した表示を再設定しますが、実際の発光復元は未測定です。
ACK損失下で左右同時・全か無かの配信は保証しません。定期更新するversion3/ls3以前とは
互換性がないため、左右ともversion4のfirmwareが必要です。

behaviorは引き続き `BEHAVIOR_LOCALITY_CENTRAL` です。両側にmoduleと同じbehavior nodeを含めてください。
時刻計算、sessionの切替、PM、メッセージとstackの費用、実機確認の境界は
[詳細な同期仕様](SCHEDULED_SYNC.md)に記載しています。

### host との接続

host のない central は、パターンの代わりに 70% で 300 ms 点灯し、その後 1.2 秒完全に
消灯する表示を行います。速度設定には従わない固定の周期です。接続状態を示すものなので、
パターンの設定にかかわらず見分けられる必要があるからです。パターンを選択すると、
次に接続状態が変わるまでこの表示は解除されます。`led.adv_blink` を無効にすると常に
解除されます。peripheral は独自のendpointを判断せず、centralが予約したadvertising flagを
他の共通表示フィールドと一緒に表示します。

## スケジューリングと消費電力

アニメーションの tick はありません。各曲線は表示する明るさ *と* その明るさが正しい
期間を返し、再描画はちょうどその時点に予約されます。blink は 1 秒に 40 回ではなく
2 回だけ起き、slow breathe の最後の 10 秒の消灯は 1 回の sleep で済み、steady は何も
予約しません。ramp だけは自身の境界を持たない形で、時計でサンプリングする唯一のもの
です。実際に描画している間だけ 40 ms 間隔で行います。

アニメーションは `zmk_activity_state_changed` にも従います。ZMK がキーボードを idle と
報告すると LED は消灯し、次の押下までアニメーションの再描画を止めます。splitの次の有効な表示変更も予約時刻で行います。精度の期限切れだけではLED通信を行いません。使用中の `idle_off` を無効に
すると別の方針になり、電源が入っている間アニメーションを続けます。この場合はアニメーションの処理が続きます。idle_offが無効なら、activityの変化だけではLED同期を始めません。判定に使う 2 つの入力は
保持せずに毎回読み直すため、キーボードが *すでに* idle のときに無効にすると、キーを
待たずに点灯を要求します。splitでは将来の共通予約時刻に適用します。

**splitの通常表示のactivityはcentralが判断します。** centralは左右のpositionを見て、
idle方針とactivityを点灯状態と一緒に予約します。復帰時の位相も共通の将来時刻から開始します。
有効な予約がないときの診断表示はlocal activityに従います。PMのlocal消灯は別のfence/drain手順で行います。

どちらのインジケーターも、blink 系パターンで使う 3-5% の下限ではなく、真に 0 の
消灯区間を使います。nRF では、Zephyr の PWM driver が PWM peripheral を停止し、
必要な高周波クロックを解放するのは、すべての channel がちょうど 0% か 100% の duty の
ときだけです。暗い下限を残すと、何かがおかしいときに出て長く続くかもしれない状態の間、
そのクロックが動き続けます。advertising インジケーターの *点灯* 区間は 70% のままです。
PWM とクロックの負荷を含む電流は、対象基板での測定が必要です。

standaloneの再描画とsplit送信はZMKの低優先度queueを使います。splitの再描画と予約実行は、
同じ低優先度の専用1536byte queueに直列化します。送信側のcore処理が待っても描画queueを占有しません。
system queueのadvertising再開処理とも分かれます。実行時stack使用量、発光時刻、消費電力は
ビルド結果から保証するものではありません。

## 互換性とテスト

| Core と役割 | 対応範囲 |
| --- | --- |
| Stock ZMK、standalone | 基本 behavior と任意の電池補正 |
| DYA、standalone | 基本 behavior と任意の custom-settings / Studio 連携 |
| DYA、split central と peripheral 1 台 | DYA relay API による予約同期。設定連携は central 側で任意 |
| Stock ZMK、split | 対象外。必要な DYA relay API がありません |

互換性の CI は upstream の `main` と `cormoran/zmk` の `main+dya` を追跡します。
既定の DYA fixture は旧 registry を固定し、現行 registry は明示的な追加の override
として検証します。固定した offline 依存関係とコマンドは[テストガイド](tests/README.md)にあります。

CI は stock standalone、DYA standalone、DYA central、DYA peripheral の
4 つの nRF52840 fixture をビルドします。また、固定した Zephyr の実カーネル上で
relay / PWM を制御した両役割の native 検証を行います。firmware 検証では
3 つの behavior と、設定連携が有効な場合の namespace・9 個のキーがリンクされること、PWM LED の所有者が 1 つになるよう `CONFIG_ZMK_BACKLIGHT` が無効の
ままであること、ZMK の電池報告が残量 % の取得モードで、リンクされた電池ラッパーを
`chosen` の `zmk,battery` として読むことを確認します。

LED 点灯で電池 ADC が低く出る基板向けに、任意の
`zmk,led-battery-adc-offset` センサを追加できます。`source-sensor` に
`zmk,battery-voltage-divider`(`<&vbatt>` など)、`full-duty-offset-mv = <50>` を
設定し、`chosen` の `zmk,battery` をこのセンサへ向けます。分圧センサの取得後、
その時点で記録されている、直近の成功したローカル LED 書き込みの duty に応じて
0～50 mV を加算します。PWM の変化と ADC の取得時点は一括して記録されないため、
これは負荷補正の推定値です。取得中に点灯状態が変わると、記録した duty と ADC が
測った負荷が異なる場合があります。電気的な精度は実機での測定が必要です。
記録された duty が 0 の場合は加算しません。ZMK が報告するのは残量 % で、
分圧センサはそれを補正前の電圧から
計算するため、このセンサは補正後の電圧から分圧センサと同じ方法で % を計算し直します。
DYA フォークでは同じノードの `mv-to-pct-thresholds` で補間し、しきい値を持たない
upstream ZMK の分圧センサでは ZMK の固定のリチウムイオン直線(3450 mV で 0%、
4200 mV で 100%)を使います。`SENSOR_CHAN_GAUGE_VOLTAGE` と
`SENSOR_CHAN_GAUGE_STATE_OF_CHARGE` の両方を返します。BAS、split peripheral の
残量、ディスプレイのすべてに補正後の値が届きます。補正は分圧抵抗値によるベースの
固定較正に上乗せされます。50 mV は実測値に合わせて変更し、中間の明るさも実機で
確認してください。% の変換は 2 方式とも 0～5 V の全 mV で ZMK 自身のコード(float の補間と
リチウムイオン直線)と一致することを確認し、実際のコールバックは実物同様に gauge 以外の
チャンネルを拒否するスタブに対して
取得失敗・読み出し失敗・0/50/100% の場合を故障注入で試し、CI で行・分岐 100% を
要求します。ハードウェア実測の代わりにはなりません。

host 回帰テストは `bash ./tests/run-host-docker.sh` で ASan/UBSan を実行し、full mode では
最適化・coverage の構成も保持します。設定の即時反映と項目間の独立性、後からの client 編集、
通知処理中の更新、制御された dispatch 失敗と再試行、保存時の現在値維持、メモリー失敗と回復、
USB ポート開閉を現行・旧 API の両方で確認します。元の入力順序を維持した決定的な
10 万ステップの通知世代テストも再生し、小さな `usb_pending.h` helper の行・
分岐カバレッジ 100% を CI の条件にします。host ハーネスは、controller・設定ブリッジ・
電池・behavior の対象関数を確認します。18 種類のカーブ、BLE あり・なしの単体構成、
split controller API も対象です。full mode は、小さな helper と対象 runtime 関数について、
テストガイドに記載した行・分岐カバレッジの基準を適用します。実際の scheduled adapter と
kernel の並行処理は native テストで別に確認します。devicetree からのインスタンス生成、
relay の登録、設定の宣言は firmware fixture で確認します。

現行のコマンド、置き換えた assertion の対応表、実機 acceptance の要件は
[module 所有のテストガイド](tests/README.md)を参照してください。

USB 向けの抑制策が対象にするのは、このモジュールから生じた LED 設定通知です。
DYA 共通の RPC 送信部は変更しないため、別の Studio 通知元まで含めて送信バッファ
満杯時の停止を防ぐ保証はありません。

## split を ls3 から ls4 へ更新する

1. 左右とも同じ module revision を選び、対応する DYA core で両方の firmware を
   ビルドします。再現できるよう、利用側の manifest にはその revision を固定します。
2. 左右を一組として更新します。Version4 / `ls4` は version3 / `ls3` 以前の相手を
   意図的に受け付けないため、一時的に異なる版が混ざると通常表示を同期できません。
   古い wire 形式への自動 fallback はありません。
3. 再接続し、起動時、連続した設定操作、idle / wake、片側だけの suspend / resume を
   確認します。この wire 更新のために LED キー・設定を移行したり、設定・bond を
   リセットしたりする必要はありません。

desired 設定は即時更新し、split の表示は将来の共通時刻に切り替えます。有効な変化が
ない間は時計の期限後も COASTING として表示を続けるため、長時間の実際の位相一致は
保証しません。有限回の回復処理と精度の条件は [SCHEDULED_SYNC.md](SCHEDULED_SYNC.md) を参照してください。

release 前には、対象の実機・production firmware で split / BLE の配信、混雑・再接続、
PWM 時刻、消費電力、USB / Studio の共存、reset / PM の保持を確認してください。
host / native 検証やビルド成功は、これらの実機 acceptance の代わりにはなりません。

## License

[MIT](LICENSE)
