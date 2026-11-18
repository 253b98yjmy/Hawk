# Hawk

[![CI](https://github.com/253b98yjmy/Hawk/actions/workflows/ci.yml/badge.svg)](https://github.com/253b98yjmy/Hawk/actions/workflows/ci.yml)

C で一から書く、軽量・プライバシー重視の Web ブラウザ（JavaScript なし）。
設計は [docs/design/](docs/design/README.md) を参照。

## 必要なもの

- Windows 11 x64
- Visual Studio Build Tools（「C++ によるデスクトップ開発」ワークロード。Ninja も同梱）
- CMake 3.25 以上

## ビルド

```bat
scripts\build.cmd debug      :: Debug + AddressSanitizer。ビルド後にテストを実行
scripts\build.cmd release    :: Release（LTO）。ビルド後にテストを実行
scripts\build.cmd fuzz       :: ファジング用の実行ファイルをビルド
```

成果物は `build\<プリセット名>\` に出力されます。

- 実行：`build\release\src\hawk.exe`
- 起動時間とメモリの計測：`build\release\src\hawk.exe --smoke 2> log.txt`
- ファジング：`build\fuzz\fuzz\fuzz_utf8.exe -max_total_time=60`

## ライセンス

GPL-3.0-or-later。全文は [LICENSE](LICENSE) を参照。
