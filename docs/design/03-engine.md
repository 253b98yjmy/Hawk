# 03. レンダリングエンジン

## 1. 文字コード判定

判定の優先順位（WHATWG Encoding 標準に従う）：

1. BOM
2. HTTP の `Content-Type` の charset
3. 先頭 1024 バイト内の `<meta charset>` / `<meta http-equiv>`
4. 推測（日本語向けに、UTF-8、Shift_JIS、EUC-JP、ISO-2022-JP を判別する簡易検出器を持つ）
5. 既定値は windows-1252（ロケールが日本語なら Shift_JIS）

- UTF-8 と windows-1252 は自前で変換する。
- それ以外の文字コードは `MultiByteToWideChar`（コードページ 932、20932、50220、936、949、950 など）でいったん UTF-16 にし、さらに UTF-8 へ変換する。
- 不正なバイト列は U+FFFD に置き換える。

## 2. HTML パーサ

- WHATWG HTML 標準のトークナイザとツリー構築を、**省略せずに**実装する。
  - 対象には、adoption agency アルゴリズム、foster parenting、挿入モードのすべてを含む。
  - 実際のサイトの HTML は壊れていることが多く、仕様どおりに直さないと表示が崩れるため。
- **スクリプト無効（scripting flag = off）で解析する**：`<noscript>` の中身を通常の HTML として扱う。
- 名前付き文字参照の表は、`tools/gen_entities.py` で生成したソート済み配列として埋め込む（`src/html/hk_html_entities.inc`）。最長一致は、候補の長さを短くしながら二分探索する（名前は最長 32 文字なので十分速い）。
- `<script>` の中身は解析するが、DOM には保存せず捨てる（メモリ節約）。
- `<template>` の中身は、描画しない断片として保持するだけにする。
- **パース中に集めておく情報**（DOM は後から変わらないので、ここで集めれば済む）：
  - class 名と id のハッシュ集合（ブロッカーの要素非表示に使う）
  - `<link rel=stylesheet>`、`<style>`、`<img>` の一覧
  - `<base href>`、`<meta>` の内容
  - フォームとその入力欄
- 不正な入力でも停止しないことを、html5lib-tests で確認する（06 章）。

### 対応する要素の扱い

| 要素 | 扱い |
|---|---|
| 通常の文書要素 | 描画する |
| `<img>`、`<picture>`、`srcset` | 描画する（画像は 7 節） |
| `<svg>` | M5 で一部対応（`path`、`rect`、`circle`、`ellipse`、`line`、`polygon`、`g`、`fill`、`stroke`、`viewBox`）。それまでは何も描画しない |
| `<iframe>` | 描画しない。「埋め込み: ホスト名」というリンクを表示する |
| `<video>`、`<audio>` | `poster` 属性の画像と、「外部で開く」リンクを表示する |
| `<canvas>`、`<object>`、`<embed>` | 代替コンテンツ（子要素）を表示する |
| `<form>` 関連 | 9 節 |

## 3. CSS

### 3.1 パーサ

- CSS Syntax Level 3 に従ってトークナイザとパーサを実装し、エラーから回復できるようにする。
- 宣言は「プロパティ ID（静的アトム）＋ 値の構造体」に変換して、文書アリーナに保存する。
- 未対応のプロパティは、解析の時点で捨てる。メモリの節約になり、`@supports` を正しく判定するためにも必要。

### 3.2 対応範囲

| 分類 | 対応する | 対応しない |
|---|---|---|
| セレクタ | Selectors Level 3 全体、`:is()`、`:where()`、`:not(<リスト>)`、`:has()`（M5） | `:focus-visible` 以外の、状態系の新しい擬似クラス |
| アットルール | `@media`、`@import`、`@supports`、`@font-face`（既定では無効）、`@layer`（M5） | `@keyframes`（解析して捨てる）、`@container`、`@property` |
| メディア特性 | `width`、`height`、`orientation`、`prefers-color-scheme`、`prefers-reduced-motion`（常に reduce）、`hover`、`pointer`、`resolution` | — |
| 値 | 長さ（px、em、rem、%、vw、vh、vmin、vmax、pt、ch、ex）、`calc()`、`min()`、`max()`、`clamp()`、`var()`（カスタムプロパティ）、色（16 進、名前、`rgb()`、`hsl()`、`currentColor`、現行の構文）、`linear-gradient()` | `color-mix()` などの新しい色関数（M6 以降） |
| ボックス | `display`（block、inline、inline-block、flex、inline-flex、list-item、table 系、none、contents、flow-root）、margin、padding、border、`box-sizing`、width と height と min/max、`aspect-ratio`、`overflow`、`position`、`z-index`、`float`、`clear`、`visibility`、`opacity` | `display: grid`（M6。それまでは block として扱う） |
| 文字 | font 系、`line-height`、`text-align`、`text-indent`、`text-decoration`、`text-transform`、`white-space`、`word-break`、`overflow-wrap`、`letter-spacing`、`word-spacing`、`vertical-align`、`direction`（基本のみ） | 縦書き（`writing-mode`、将来課題）、`text-shadow` |
| 装飾 | 背景色、背景画像（`repeat`、`position`、`size`）、`border-radius`、`box-shadow`（M5）、リストのマーカー | `filter`、`backdrop-filter`、`mask`、`clip-path` |
| 変形・動き | `transform: translate()` のみ（M5） | アニメーション、トランジション（最終状態で描画）、その他の 3D・2D 変形 |

### 3.3 カスケード

- **出所の優先順位**：UA スタイル < ユーザー（ブロッカー）< 作者 < 作者の `!important` < ユーザーの `!important`（ブロッカーの非表示）< UA の `!important`。
  - ブロッカーの `display:none !important` は、仕様どおりの優先順位で作者の `!important` に勝つ。特別な処理は要らない。
- **ルールの索引**：一番右のセレクタ部分のキー（id → class → タグ → その他）で、ルールを振り分ける。
- **祖先のブルームフィルタ**：子孫結合子や子結合子を照合するとき、明らかに一致しない祖先を、たどる前に除外する。
- **スタイル共有**：親が同じで、タグ・class・属性・状態が同一の兄弟要素は、計算結果を使い回す。

### 3.4 計算済みスタイル

> **M2 の実装**：グループ分けの前段として、計算済みスタイル全体をハッシュして同じものを共有する（`intern_style`）。Wikipedia 日本語版「日本」では、要素 16,181 個に対して異なるスタイルは 456 種類だった。

- プロパティを、変化しやすさと継承の有無でグループに分ける。
  - 例：文字（継承する）、ボックス、背景、枠線、位置、リスト、その他。
- 各グループは、文書アリーナ上でハッシュによって重複を除き、要素からは 32 ビットの番号で参照する。
- 典型的なページでは、異なるグループの組み合わせは数百種類程度に収まる。要素ごとに全プロパティを持つより、1 桁以上小さくなる見込み。
- **再計算が必要になる場面**：`:hover`、`:focus`、`:active`、`:checked`、フォームの状態、ビューポートの変化、スタイルシートの到着だけ。
  - `:hover` を含むルールは、どの要素に影響しうるかを事前に記録しておく。マウスの移動で、全体を再計算しないようにするため。

## 4. ボックスツリーとレイアウト

> **M2 の実装状況**（`src/layout/hk_layout.c`）：
> - ボックスツリーは保持せず、DOM と計算済みスタイルから一度の再帰で描画リストを直接作る。
> - 長さは当面 `float` の CSS px で扱う（DirectWrite が `float` のため）。固定小数点への移行は、計測で必要と分かってから行う。
> - インライン整形は、1 つの段落を DirectWrite の `IDWriteTextLayout` 1 個にまとめ、要素ごとの書式を範囲として設定する。改行・双方向テキスト・フォールバック・下線は DirectWrite に任せ、グリフ列を描画リストへコピーしたら `IDWriteTextLayout` はすぐ解放する。
> - 未対応で M3 以降に回したもの：float（通常のブロックとして配置）、position: absolute / fixed（描画しない）、table（ブロックとして縦に並べる）、Flexbox / Grid（ブロックとして扱う）、画像、inline-block（中身をインラインとして展開）。

### 4.1 単位

- 内部の長さは `int32_t` の固定小数点で表す（1/64 px、`hk_lu`）。
- 小数の丸めによって、ページの見た目が実行ごとに変わらない（結果が決定的になる）。

### 4.2 ボックスツリーの生成

- `display` の値に従って、ブロックボックス、インラインボックス、匿名ボックス、table の補完、リストマーカーを生成する。
- テキストはこの段階で、`white-space` の規則に従って空白を処理する。

### 4.3 対応する整形方式

| 方式 | 内容 | 時期 |
|---|---|---|
| ブロック整形 | マージンの相殺、`clear`、`flow-root` | M2 |
| インライン整形 | 改行、`text-align`、`vertical-align`、`line-height`、インラインブロック | M2 |
| `float` | 左右の回り込み | M3 |
| `position` | `relative`、`absolute`、`fixed`。`sticky` は当面 `relative` として扱う | M3 |
| table | 自動レイアウトと固定レイアウト、`colspan`、`rowspan`、`border-collapse` | M3 |
| Flexbox | `flex-direction`、`wrap`、`grow`、`shrink`、`basis`、`justify-content`、`align-items`、`gap`、`order` | M4 |
| `overflow` によるスクロール領域 | `overflow: auto / scroll` の要素内スクロール（`pre` やコードブロックで必要） | M4 |
| Grid | 明示的なトラック、`fr` 単位、`repeat()`、`gap`、領域名 | M6 |

### 4.4 改行と字形整形

- DirectWrite の `IDWriteTextAnalyzer` を使う。
  - `AnalyzeScript`（文字体系）、`AnalyzeBidi`（双方向テキスト）、`AnalyzeLineBreakpoints`（改行可能位置）、`GetGlyphs` / `GetGlyphPlacements`（字形整形）。
- **整形結果のキャッシュ**：「テキスト範囲、フォント、サイズ」をキーに、整形結果を保存する。画面幅を変えたときは、改行位置だけを計算し直す。
- 日本語の禁則処理は、DirectWrite の改行位置の分析に任せる。

### 4.5 再レイアウトの範囲

- **再レイアウトが起きる場面**：ビューポートの変化、画像サイズの確定、スタイルシートの到着、スタイルの再計算（hover によってレイアウトが変わる場合のみ）。
- 変更のあったボックスに印を付け、そこから影響の及ぶ最も近い親までをやり直す。
- 画像は、`width` / `height` 属性または `aspect-ratio` があれば、先に領域を確保しておく。後から画面がずれるのを防ぐため。

## 5. 描画

### 5.1 描画リスト

- CSS 2.1 の付録 E（描画順序）とスタッキングコンテキストに従って、描画命令の列を作る。
- **命令の種類**：
  - 矩形の塗りつぶし、枠線（角丸を含む）、テキスト（グリフ番号と位置の配列、フォント）
  - 画像（拡大縮小と繰り返し）、グラデーション
  - クリップの開始・終了、不透明度レイヤの開始・終了
- **画面外の命令を素早く除外する**：命令は 256 px の高さの帯ごとに索引する。

### 5.2 ラスタライザ（自作）

- 対象は 32 ビット BGRA で、アルファは事前乗算する。描画先は、DIB セクションのバックバッファ 1 枚。
- **自作する機能**：
  - 矩形
  - 角丸矩形と任意パス（スキャンライン方式で、被覆率によるアンチエイリアス）
  - 画像の転送（最近傍法とバイリニア補間）
  - グリフのアルファマスクの合成
  - 線形グラデーション
- 不透明度レイヤは、その範囲だけを一時バッファに描いて合成する（使われる頻度は低い）。

### 5.3 画面への転送とスクロール

- **スクロール**：バックバッファ内の画素を `memmove` でずらし、新しく見えた帯だけを描き直す。その後 `BitBlt` で画面へ送る。
- **`position: fixed` の要素**：別レイヤとして保持し、スクロールのたびに上から合成する。
- **ちらつき防止**：`WM_PAINT` では、変更された矩形だけを転送する。
- **高 DPI**：Per-Monitor V2 に対応する。CSS の px × DPI 倍率で描画する。

## 6. テキスト描画とフォント

> **実装上の注意**：DirectWrite のヘッダ（`dwrite.h`）は C++ 専用で、C からは使えない。M2 では `plat/win32` に C++ の薄いラッパ（例外・RTTI・STL なし、C の API だけを公開）を置く。M1 の一時的なテキスト表示は GDI の `DrawTextW` で描いている（`plat/win32/hk_textview_win32.c`、M2 で削除する）。

- **既定のフォント**（設定で変更できる）：
  - sans-serif：Segoe UI → Yu Gothic UI → Meiryo
  - serif：Yu Mincho
  - monospace：Cascadia Mono → MS Gothic
- **フォールバック**：`IDWriteFontFallback::MapCharacters` を使う。
- **グリフのラスタライズ**：`IDWriteGlyphRunAnalysis::CreateAlphaTexture` でグレースケールのアンチエイリアスにする。ClearType は将来の選択肢とする（どんな背景色でも合成できるため、まずはグレースケールにする）。
- **グリフキャッシュ**：「フォント、サイズ、グリフ番号、横方向の端数位置（4 段階）」をキーにする。上限は 4 MB で、LRU で追い出す。
- **Web フォント**（設定で有効にした場合）：
  - TTF / OTF / WOFF は、DirectWrite のメモリ上フォントローダで読み込む。
  - WOFF2 は Brotli の展開が必要なので、M6 以降にする。

## 7. 画像

- **取得**：ビューポートから上下 1.5 画面分の範囲に入った時点で取得する。全画像を遅延読み込みする。
- **`srcset` / `<picture>`**：表示サイズ × DPI を満たす候補のうち、最も小さいものを選ぶ。
- **展開**：WIC を I/O スレッドで使う。`IWICBitmapScaler` で **表示サイズに縮小しながら**展開する。
  - 例：4000×3000 の写真を 800×600 で表示する場合、展開後のメモリは約 48 MB ではなく約 1.9 MB で済む。
- **展開済み画像のキャッシュ**：全体の上限は 64 MB。画面から遠いものから捨て、再び必要になったら展開し直す。
- **アニメーション GIF**：最初のフレームだけを表示する（設定で動かせるようにするのは将来課題）。
- **対応形式**：PNG、JPEG、GIF、BMP、ICO、WebP（WIC にコーデックがある場合）。SVG は 2 節のとおり。

## 8. ナビゲーション

- リンク、フラグメント（`#id` への移動）、戻る・進む（スクロール位置とフォームの入力内容を復元）、再読み込み、停止。
- **ページ内検索（Ctrl+F）**：DOM のテキストを順に照合し、該当箇所を強調描画する。
- 新しいタブで開く：`target=_blank` のリンク、中クリック、Ctrl+クリック。
- `Content-Type` が表示できない種類の場合は、ダウンロードとして扱う（05 章）。

## 9. フォーム

JS がなくても、HTML フォームの送信は動く。検索やログインに必須の機能。

- **対応する部品**：
  - `input`（text、search、password、email、url、number、hidden、checkbox、radio、submit、reset、image）
  - `textarea`、`select`（単一選択と複数選択）、`button`、`label`
- **送信形式**：GET と POST（`application/x-www-form-urlencoded`）、`multipart/form-data`（M5。ファイル選択を含む）。
- **部品の描画**：OS のコントロールは使わず、自前で描く。見た目が揃い、軽量なため。
- **テキスト編集**：自作する。
  - 単一行は M3、`textarea` は M4 で対応する。
  - キャレット、選択範囲、クリップボード、取り消しに対応する。
  - **IME**：IMM32 を使う（`WM_IME_STARTCOMPOSITION` / `WM_IME_COMPOSITION`、候補ウィンドウの位置指定）。日本語入力に必須。TSF への移行は将来課題。
- **入力検証**：`required`、`pattern`、`type=email` などを、送信時に検証する（HTML 標準の制約検証）。
- **自動補完・パスワード保存**：持たない（プライバシーと、実装の簡潔さのため）。
