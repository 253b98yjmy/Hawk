# 02. ネットワークとプライバシー

## 1. URL

- WHATWG URL 標準に従ってパーサを実装する。`http`、`https`、`data`、`hawk`（内部ページ）、`file`（読み取り専用）に対応する。
- 国際化ドメイン名は、Windows の `IdnToAscii` / `IdnToUnicode` で変換する。
- **なりすまし対策**：アドレスバーには、複数の文字体系が混在するホスト名を Punycode のまま表示する。Chromium の IDN 表示方針を簡略化して採用する。
- `javascript:` の URL は無視する。リンクは描画するが、クリックしても何もしない。

## 2. Public Suffix List（PSL）

- ビルド時に、PSL を DAFSA（コンパクトな有限オートマトン）形式に変換して埋め込む（`tools/gen_psl`、約 40 KB）。
- PSL は次の 3 か所で使う：**eTLD+1 の判定**（ブロッカーのサードパーティ判定、Cookie の Domain 属性の検証、キャッシュの分離）。
- 更新はブラウザのアップデートに含める。

## 3. 名前解決（DNS）

- 既定は、OS のリゾルバを非同期で呼ぶ（`GetAddrInfoExW` と完了コールバック）。
- メモリ上に DNS キャッシュを持つ（最大 256 件。TTL は OS から取れないため、一律 60 秒）。
- **DNS over HTTPS（M6 以降）**：自前の HTTP スタックの上に実装する。設定で接続先を選べるようにする。
- 先読みの名前解決（DNS prefetch）はしない。

## 4. TLS

- SChannel（`AcquireCredentialsHandle` / `InitializeSecurityContext`）を使う。TLS 1.2 と 1.3 に対応し、1.0 と 1.1 は無効にする。
- **ALPN**：M5 までは `http/1.1` だけを提示する。
- **証明書の検証**：SChannel の既定の検証に加えて、`CertGetCertificateChain` と `CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL)` でホスト名を検証する。
- **失効確認**：キャッシュ済みの情報だけを使う（オンラインで OCSP に問い合わせない）。CA に閲覧先が漏れるのを防ぐため。
- **証明書エラー**：エラーページを表示する。「理解したうえで続行」はセッション中だけ、ホストごとに許可する。
- 抽象化の境界は `plat/tls.h`：`hk_tls_connect / read / write / close`。

## 5. HTTP/1.1

- **接続プール**：ホストごとに最大 6 接続、全体で 32 接続。keep-alive を使い、アイドル状態の接続は 30 秒で切断する。パイプラインは使わない。
- **応答の解析**：状態遷移による逐次パーサ。chunked 転送と Content-Length の両方に対応する。
  - ヘッダは合計 64 KB まで、1 行 8 KB まで。超えた場合はエラーにする。
- **圧縮**：`Accept-Encoding: gzip, deflate`。zlib で逐次展開する。展開後のサイズに上限（64 MB）を設け、圧縮爆弾を防ぐ。
- **リダイレクト**：最大 20 回。リダイレクト先にもブロッカーを適用する。
- **タイムアウト**：接続 15 秒、無通信 30 秒。
- **優先度**：本文の HTML、CSS、画面内の画像、画面外の画像の順に送信を開始する。
- **HTTP/2（M6 以降、任意）**：HPACK と多重化の実装量に対して効果を見て判断する。

### 送信するヘッダ（既定）

```
User-Agent: Windows 版 Firefox ESR と同じ文字列（README の D2。更新ごとに最新の ESR に合わせる）
Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8
Accept-Language: ja,en-US;q=0.7,en;q=0.3
Accept-Encoding: gzip, deflate
Sec-GPC: 1
```

- 送らないヘッダ：Client Hints（`Sec-CH-*`）、`DNT`、`X-Requested-With`。
- **Referer**：既定のポリシーを `strict-origin-when-cross-origin` より厳しくする。
  - クロスサイトへの遷移では、送信元のオリジン（`https://host/` まで）だけを送る。
  - HTTPS から HTTP へ移るときは送らない。
  - 設定で「常に送らない」も選べる。

## 6. HTTPS のみモード

- 既定で有効。`http://` の URL は、まず `https://` に置き換えて接続を試みる。
- 失敗した場合は、警告ページで「HTTP で開く（このサイトはセッション中のみ許可）」を選べるようにする。
- サブリソースの混在コンテンツ（HTTPS のページ内の HTTP 画像など）は、すべて遮断する。

## 7. Cookie

- RFC 6265bis に準拠する。SameSite が未指定の場合は `Lax` として扱う。
- **サードパーティ Cookie は完全に拒否**する。トップレベルのサイトと eTLD+1 が異なるリクエストでは、Cookie を送信も保存もしない。
- **保存**：`%LOCALAPPDATA%\Hawk\cookies.tsv` に保存する。一時ファイルに書いてから `ReplaceFileW` で差し替え、5 秒の遅延でまとめて書き込む。
- セッション Cookie は保存しない。
- **設定**：サイトごとの削除、終了時に全削除、サイトごとの許可・拒否リスト。
- **上限**：1 ドメインあたり 50 個、全体で 3000 個。

## 8. HTTP キャッシュ

- **分離キー**：`(トップレベルサイトの eTLD+1, URL)` の二重キーにする。サイトをまたいでキャッシュの有無を調べる追跡を防ぐため。
- **メモリキャッシュ**：16 MB。現在のページのサブリソースと、戻る・進むの操作に使う。
- **ディスクキャッシュ**：`%LOCALAPPDATA%\Hawk\cache\`。キーの SHA-256 を名前にしたファイルと、索引ファイル 1 つで管理する。上限は 256 MB で、古いものから削除する。
- `Cache-Control`、`Expires`、`ETag`、`Last-Modified` による再検証に対応する。`Vary` はキーに含める。
- 休止したタブの復元（01 章 4.4 節）にも、このキャッシュを使う。
  - そのため、本文の HTML は `no-store` でない限り、タブが残っているあいだはキャッシュに保持する。
  - `no-store` の HTML は、休止せず「背景」状態のまま保持する。

## 9. ローダ（取得処理の窓口）

各リクエストは、次の順に処理する。

1. URL を正規化する（HTTPS のみモード、`$removeparam` によるパラメータ削除）
2. **ブロッカーで判定する**：止める場合は、ネットワーク処理を一切開始しない
3. 混在コンテンツを確認する
4. キャッシュを参照する
5. 接続して送受信する
6. 応答ヘッダを処理する（Cookie、リダイレクト、Content-Type）
7. 本文を逐次展開して、メインスレッドへ渡す

### リクエストの種類（ブロッカーの判定にも使う）

| 種類 | 発生源 |
|---|---|
| `document` | トップレベルのナビゲーション |
| `stylesheet` | `<link rel=stylesheet>`、`@import` |
| `image` | `<img>`、CSS の背景画像、favicon |
| `font` | `@font-face`（有効にした場合のみ） |
| `other` | 上記以外 |

iframe、script、XHR、media の各リクエストは、Hawk では発生しない。

## 10. その他のプライバシー方針

- テレメトリ、クラッシュレポートの自動送信、利用統計の収集は**一切持たない**。
- 起動時に自動で通信しない。フィルタリストの更新は、ユーザーが有効にしたリストだけを、4 日ごとに取得する。
- `<link rel=prefetch|preconnect|dns-prefetch|preload>` は無視する。
- `<a ping>`、`navigator.sendBeacon` 相当の機能は持たない。
- `:visited` は文字色にだけ適用し、背景画像などの取得には一切関係させない。
- favicon は `<link rel=icon>` で指定されたものだけを取得する。`/favicon.ico` を推測して取得することはしない。
- `<meta http-equiv=refresh>`：0 秒以上の遅延なら従う。別サイトへ移る場合はバーで通知して、キャンセルできるようにする。
