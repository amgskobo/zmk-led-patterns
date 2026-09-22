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
  専用の画面なしで編集できるようにします。その場合は設定が唯一の所有者になり、
  キー押下は設定を書き込み、LED はその設定に従います。
- アニメーション用のタイマーはありません。各パターンが次に変化する時刻を返し、
  その時点に再描画を予約します。キーボードが idle の間は何も予約しません。

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

明るさ 0 は暗い状態ではなく、LED の低消費電力状態です。段階的に下げて到達することも
できます。LED には一度だけ 0 を書き込み、PWM peripheral を停止させ、明るさが再び
上がるまで再描画を一切予約しません。上がったときは、パターンの時計が進んだ位置から
再開します。明るさ 0 でも点灯するのは split の接続インジケーターだけです。これは
診断用であり、LED の設定に関係なく同じ見た目である必要があるからです。advertising
インジケーターはパターンと同じく明るさに応じて変わるため、0 では消灯します。

電源オフのときも LED を消灯します。ZMK の `&soft_off` とアイドルスリープはイベントを
出さず、SoC が止まっている間もピンはそのレベルを保つため、その瞬間に点いていた LED は
ソフトオフ中も点いたままでした(トランジスタ経由なら、解放されたピンが残したゲートの
電荷で同じことが起きます)。`CONFIG_PM_DEVICE`(soft off が選択します)があると、
このモジュールは小さな電源管理デバイスを 2 つ登録します。2 つの電源オフ経路はデバイスを
逆の順序で suspend するため、1 つは init 順で PWM ドライバーより前、もう 1 つは LED
デバイスより後に置きます。先に動いた方がアニメーションと split のミラーを止め、LED
ドライバーがまだ動いているうちに 0 を書き込みます。もう一方はそれが済んでいるのを
確認するだけです。電源オフが中止されて resume した場合は、パターンの時計が進んだ位置
から再描画します。優先度は `CONFIG_ZMK_LED_PATTERNS_PM_INIT_PRIORITY`(45)と
`CONFIG_ZMK_LED_PATTERNS_PM_LATE_INIT_PRIORITY`(95)です。

速度は、各パターンが描かれた速さに対する百分率です。パターンごとではなく
アニメーションの時計に適用するため、1 つの設定で 18 種類すべてが同じ倍率で速くなり、
特定のパターンだけおかしくなることはありません。

`CONFIG_ZMK_LED_PATTERNS_CUSTOM_SETTINGS=y` の場合、binding は LED を直接変更しません。
現在接続中の transport について behavior の名前に対応する設定へ書き込み、LED はその
設定に従います([Studio 設定](#studio-設定)を参照)。この option がなければ、binding は
値を LED へ直接適用します。

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

`LED_PATTERN_COUNT` は最後の番号の次の値です。パターンを追加するときは、これを
増やして曲線を追加します。切り替えコマンドと設定の範囲は自動で追従します。

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

`idle_off` が transport ごとなのは、この理由が最も強く当てはまるからです。USB は
ケーブルであり、idle 中も LED を点けておくコストは問題になりません。BLE は電池であり、
この module の中で実際に使用時間を縮める唯一の設定です。

パターンが名前付きの dropdown ではなく 0 - 17 の数値なのは、Studio RPC の schema が
options 制約を 8 個までに制限し、handler が何も言わずに切り詰めるからです。18 個の
リストは、最初の 8 個だけが表示され残りの 10 個が失われた状態で client に届きます。
完全な名前は、収まる場所(keymap editor が表示するパラメーター metadata と公開
ヘッダー)にあります。

**キー押下はこれらの設定に書き込みます。** 設定が値の唯一の所有者です。キーを押すと、
使用中の transport について behavior の名前に対応する設定へ書き込み、LED は client の
編集と同じ変更 event を通じて追従します。そのため client には押下がその場で反映され、
transport の切り替えや他の値の編集で押下の結果が戻ることはありません。書き込みは
すぐにメモリーへ反映され、最後の押下から 3 秒後に flash へ保存されます。PERSIST の
書き込みは独自の debounce なしで flash へ書かれるため、連続して押すと、設定ごとに
1 回ではなく押下ごとに 1 回 flash へ書き込むことになるからです。

これらを登録するのは central だけです。peripheral には独自の設定は不要です。
central に届いたもの(キー押下または client での編集)は、最終状態として peripheral
へ mirror されます。

option が無効の場合(既定であり、upstream ZMK でのビルドもこの状態です)、この部分は
何もコンパイルされず、keymap の binding だけが操作手段になります。

## インジケーター

選択中のパターンより優先して表示されるものが 2 つあります。どちらも装飾より緊急だから
です。split インジケーターは advertising インジケーターより優先されます。

### split の接続

split では、LED は何よりもまず状態表示です。左右とも同じ 3 段階を表示し、両者が
実際に合意するまで、どちらもインジケーター表示を終えません。

| 段階 | LED | 意味 |
| --- | --- | --- |
| waiting | 点滅(パターン 3 の形) | もう片方が見つからない |
| linked | 常時点灯(パターン 0 の形) | 接続済みだが、状態はまだ合意していない |
| synced | 通常のパターン(既定は breathe (1)) | peripheral が表示内容を確認した |

接続から 5 秒後(central が peripheral の relay characteristic を検出し終える時間です。
それより前の書き込みは、ログが 1 行出るだけで transport に捨てられます)、central は
状態全体(パターン、明るさ、速度、advertising flag、現在の曲線のどこまで進んだか)を
載せた `led` packet を送ります。これにより peripheral はアニメーションを最初から
やり直すのではなく、位相を合わせて引き継ぎます。

peripheral は、現在表示しているパターンを示す `lea` packet で応答します。この確認応答
によって「synced」は推測ではなく観測された事実になり、左右両方のインジケーター表示が
解除されます。接続時の送信は、0.5 秒ずつ応答を待ちながら最大 5 回まで行い、それでも
応答がなければ諦めて常時点灯のままにします。これにより、一度も接続できなかった
組み合わせと、接続したが合意できなかった組み合わせを見分けられます。relay event の
名前は NUL を含めて 4 byte までなので、どちらも 3 文字です。

その後の状態変化は 1 件につき 1 packet を送り、20 ms の間にまとめます。設定の適用や
slider のドラッグは、連続した送信ではなく 1 回の無線 event になります。

behavior は `BEHAVIOR_LOCALITY_CENTRAL` です。peripheral が自分で behavior をもう一度
呼び出すのではなく、central が状態を所有して結果を mirror します。両方のファームウェア
image に module と同じ behavior node を含めてください。

### host との接続

host のない central は、パターンの代わりに 70% で 300 ms 点灯し、その後 1.2 秒完全に
消灯する表示を行います。速度設定には従わない固定の周期です。接続状態を示すものなので、
パターンの設定にかかわらず見分けられる必要があるからです。パターンを選択すると、
次に接続状態が変わるまでこの表示は解除されます。`led.adv_blink` を無効にすると常に
解除されます。peripheral は自分の endpoint を持たないため、この表示は出ません。

## スケジューリングと消費電力

アニメーションの tick はありません。各曲線は表示する明るさ *と* その明るさが正しい
期間を返し、再描画はちょうどその時点に予約されます。blink は 1 秒に 40 回ではなく
2 回だけ起き、slow breathe の最後の 10 秒の消灯は 1 回の sleep で済み、steady は何も
予約しません。ramp だけは自身の境界を持たない形で、時計でサンプリングする唯一のもの
です。実際に描画している間だけ 40 ms 間隔で行います。

アニメーションは `zmk_activity_state_changed` にも従います。ZMK がキーボードを idle と
報告すると LED は消灯し、次の押下まで何も予約しません。使用中の `idle_off` を無効に
すると別の方針になり、電源が入っている間アニメーションを続けます。これは、見た目だけ
でなく実際に電池を消費するこの module で唯一の設定です。判定に使う 2 つの入力は
保持せずに毎回読み直すため、キーボードが *すでに* idle のときに無効にすると、キーを
待たずにすぐ点灯します。

**split では、activity の判断は central が行います。** 左右は同じキー押下を見ている
わけではありません。ZMK は `zmk_position_state_changed` から activity を更新しますが、
peripheral の押下は左右両方に届く(自身にはローカルに、central には split 経由で)一方、
central の押下は central にしか届きません。そのため peripheral が自分の activity だけで
判断すると、もう片方で入力している間に idle timeout で消灯し、そのままになります。
そこで mirror は flag と一緒に central の activity を運び、peripheral はどちらか一方の
判断で十分とみなします。central の判断は peripheral に届かない入力を補い、peripheral
自身の判断は relay を待たずに自分のキーを反映し、問い合わせる central がない場合も
補います。

その代償は idle の 1 周期あたり 2 通の追加メッセージです。どちらもパターンの位相を
運ぶため、復帰時には起きていることだけでなく曲線の位置も再び合意します。

どちらのインジケーターも、blink 系パターンで使う 3-5% の下限ではなく、真に 0 の
消灯区間を使います。nRF では、Zephyr の PWM driver が PWM peripheral を停止し、
必要な高周波クロックを解放するのは、すべての channel がちょうど 0% か 100% の duty の
ときだけです。暗い下限を残すと、何かがおかしいときに出て長く続くかもしれない状態の間、
そのクロックが動き続けます。advertising インジケーターの *点灯* 区間は 70% のままです。
ここでは LED 自身の電流が支配的で、暗くするほうが消費電力を抑えられるからです。

すべての再描画と mirror は system work queue ではなく
`zmk_workqueue_lowprio_work_q()` で実行します。ZMK は advertising の再開処理を system
work queue に投入するため、アニメーションが、host 切断後にキーボードを再び見つけられる
ようにする処理の前に並ぶべきではありません。また、Zephyr は呼び出し元がその queue の
とき ATT TX buffer の確保で待たないため、そこから投入した split の書き込みは buffer
pool が一時的に空になるたびに失敗します。

## 互換性とテスト

基本の behavior は upstream ZMK の `main` でビルドできます。任意の Studio 設定連携には、
追加で `cormoran/zmk` の `main+dya` と `zmk-feature-custom-settings` が必要です。option を
無効にすればその依存はなくなります。

CI は両方の構成で nRF52840 向けの実際の ZMK ファームウェア fixture をビルドします。
3 つの behavior がすべてリンクされていること、DYA の設定 namespace とキーが DYA ビルドに
だけ含まれること、PWM LED の所有者が 1 つになるよう `CONFIG_ZMK_BACKLIGHT` が無効の
ままであることを確認します。

## License

[MIT](LICENSE)
