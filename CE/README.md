<!--
SPDX-License-Identifier: MIT
Copyright (c) 2026 daizu-coder
-->

# PopGBA

PopGBA is a Game Boy Advance emulator for the **SHARP Brain PW-G5300**
electronic dictionary (Windows CE / ARM), built by wrapping the
**gpSP** (gameplaySP) libretro core in a native Win32 frontend.

## 概要

- SHARP の電子辞書 **Brain PW-G5300**（Windows CE / ARM）向けに、GBA エミュレータ **gpSP**（gameplaySP）の libretro コアをネイティブ Win32 フロントエンドで包んだ移植版です
- 無料・非営利のホームブリュー（自作ソフト）で、ソースコードを同梱しています
- ライセンスは、全体が **GPL-2.0**(上流の gpSP と同じ条件)、自作部分が **MIT** です。詳細は [`LICENSE`](LICENSE)・[`COPYING`](../COPYING)・[`THIRDPARTY_LICENSES.txt`](THIRDPARTY_LICENSES.txt)・[`LICENSING.md`](LICENSING.md) を参照してください
- ゲーム ROM・GBA 実機 BIOS（`gba_bios.bin`）は同梱していません。利用者が合法的に用意したものを使用してください。BIOS を用意しなくても、ゲームは動きます。
- SHARP・任天堂とは一切関係のない非公式のファンプロジェクトです
- gpSP の公式版ではありません。gpSP の作者・メンテナーはこの移植に関わっていないので、不具合はこちらに報告してください

## ダウンロード

ビルド済みの実行ファイルは Releases のページからダウンロードできます。
https://github.com/daizu-coder/PopGBA/releases/latest

## ビルド方法

- SDK / トゥールチェーン
  * cegcc（`arm-mingw32ce-*`）— Windows CE / ARM 向けクロスコンパイラ
- ビルド手順
  * `CE/` ディレクトリに移動し、`make && make strip` を実行（エミュレータ本体は、リポジトリ直下の上流 gpSP のソースをそのまま使います）
  * `CE/AppMain.exe` が生成されます（依存 DLL は `COREDLL.dll` のみ）

## 使用方法

対象は SHARP Brain(PW-G5300)。PC にリムーバブルディスクとして接続し、ドライブ直下に次の構成を作ります(メニュー項目名は機種により異なる場合があります):

```
<ドライブ直下>/
  アプリ/
    <任意のアプリ名>/
      AppMain.exe    ← ビルド生成物をそのまま
      index.din      ← 中身は空でよいダミーファイル
```

- ROM ファイルは SD カード上に置いてください。アプリ内の「ROMを開く」から選べます
- `index.din` をこの名前で置くと、そのフォルダが [追加アプリ・動画] に一覧表示されます
- 設定ファイル `PopGBA.cfg` は、`AppMain.exe` と同じフォルダに作られます。設定ファイルが無いときの既定値は、UI 言語=日本語、デバッグログ=OFF、画面の表示倍率=x2 です
- `PopGBA_debug.log` は Video Config で「デバッグログを有効にする」を ON にしたときのみ生成されます(既定は OFF)
- セーブデータ(`.srm`)とステートセーブ(`.state`)のファイルは、ROM と同じフォルダに作られます
- BIOS
  * ゲームの ROM と、ゲームボーイアドバンス本体の BIOS（任天堂の著作物）は同梱していません。BIOS を用意しなくても、ゲームは動きます。`AppMain.exe` の中に、オープンソースの BIOS が入っているためです。本物の BIOS を使うときは、ご自身で用意したものを `gba_bios.bin` という名前で置いてください（詳しくは [`LICENSING.md`](LICENSING.md)）
  * 置く場所は `AppMain.exe` と同じフォルダです。そこに無いときは、開いた ROM と同じフォルダも探します
  * 見つからないときや、中身が正しくないときは、オープンソースの BIOS を使います
- ROM ファイル名・ROM を置くフォルダ名・`AppMain.exe` を置くフォルダ名・BIOS を置くフォルダ名は、いずれも日本語を含んでいても開けます

## 対応状況

セーブは、SRAM / フラッシュ / EEPROM に対応しています。

時計(RTC)を使うゲームに対応しています。時計の動作は確かめていません。

次の機能には対応していません：振動、通信ケーブル、ワイヤレスアダプタ、傾きセンサー、光センサー(『ボクらの太陽』など)、ジャイロセンサー(『まわる メイドインワリオ』)、カードeリーダー。

## 動作確認環境

- SHARP Brain PW-G5300

## クレジット

- **gpSP（gameplaySP）** GBA エミュレーションコア — 原作者は **Exophase** 氏。GPL-2.0
- **libretro / gpsp** — 上記を libretro 化・保守しているフォーク（**David Guillen Fandos** 氏および libretro チーム）<https://github.com/libretro/gpsp>
- **オープンソースの GBA BIOS**（`open_gba_bios.bin`）— 任天堂の公式 BIOS の代わりとなるもの。**Normmatt** 氏 / **VBA・VBA-M 開発チーム** 製、GPL-2.0
- **ARM 命令エンコードマクロ** — Mono プロジェクトの ARM コードジェネレータ。**Sergey Chaban** 氏 / **Wild West Software**、MIT
- **libretro API** ヘッダ・**libretro-common** の一部 — **The RetroArch team**、MIT
- **Galmuri** ビットマップフォント（メニューの既定の文字）— **Lee Minseo**（quiple）氏、SIL Open Font License 1.1 <https://github.com/quiple/galmuri>
- **東雲（しののめ）16 ドットビットマップフォント** — メインデザイン **古川 泰之** 氏ほか、**The Electronic Font Open Laboratory（/efont/）**。実質パブリックドメイン <https://github.com/code4fukui/shinonome-font>
- **CeGCC** — Windows CE / ARM 向けクロスコンパイラ。**Danny Backx** 氏ほか、モダン版の **Max Kellermann** 氏
- **SHARP Brain homebrew コミュニティ** — 端末固有情報を残してくださった皆さん
- Windows CE フロントエンド（`CE/`）は本プロジェクトで作成

コンポーネントごとの出所とライセンスの詳細は [`THIRDPARTY_LICENSES.txt`](THIRDPARTY_LICENSES.txt) および [`LICENSING.md`](LICENSING.md) を参照してください。

## 制作について

コードとマスコットの絵はAI(Claude)で作りました。製作者はプログラムを読めません。

マスコットの絵とアイコン(`CE/icon/`)は CC0 1.0(パブリックドメイン)です。アイコンは、Pop シリーズのマスコットをもとに AI(Claude)で作りました。各ライセンスについては、[`LICENSING.md`](LICENSING.md) をご覧ください。

## ライセンス

PopGBA 全体は、上流の gpSP と同じ条件の **GNU General Public License バージョン2(GPL-2.0)** で配布します。本文はリポジトリ直下の [`COPYING`](../COPYING) にあります。

- ソース・バイナリとも再配布できます(GPL の範囲で商用利用もできます)。ライセンスの文と各ファイルの著作権表示は残してください
- バイナリを配るときは、対応するソース一式(このリポジトリ)も入手できるようにしてください
- 改変したものも、全体を GPL で配布してください
- 保証はありません

PopGBA の自作部分(`SPDX-License-Identifier: MIT` と書いてあるファイル)は MIT です([`LICENSE`](LICENSE) の2節)。バイナリを配るときは、[`THIRDPARTY_LICENSES.txt`](THIRDPARTY_LICENSES.txt) も一緒に配ってください。同梱コンポーネントは GPL-2.0・MIT・SIL OFL 1.1・CC0 1.0・実質パブリックドメインのいずれかで、非営利限定の条項を持つものは含まれません。

## 商標・免責

PopGBA は非公式のファンプロジェクトです。シャープ株式会社および任天堂とは一切関係がなく、許諾・後援・承認も受けていません。

- 「SHARP」「Brain」はシャープ株式会社の商標です。
- 「Game Boy Advance」「ゲームボーイアドバンス」「Nintendo」「任天堂」は任天堂の商標です。
