# PopGBA のライセンス

PopGBA は、ゲームボーイアドバンスのエミュレータ [gpSP](https://github.com/libretro/gpsp)(Exophase 氏作、libretro 版)を、SHARP Brain PW-G5300(Windows CE)向けに移植した**非公式**の改変版です。gpSP の作者やメンテナーはこの移植に関わっていません。

ライセンスは2段になっています。

## 1. アプリ全体: GNU GPL バージョン2(上流と同じ条件)

アプリ全体(`AppMain.exe`、およびこのリポジトリで公開しているソース全体)は、上流の gpSP と同じ条件の **GNU General Public License バージョン2(GPL-2.0)** で配布します。GPL バージョン2の本文はリポジトリ直下の [`COPYING`](../COPYING) に、gpSP の著作権表示は [`LICENSE`](LICENSE) の3節にあります。

「それ以降のバージョンの GPL」も選べるかどうかは、ここでは断定しません。上流のファイルの書き方がそろっていないためです。gpSP の多くのソースは「バージョン2、または(選んで)それ以降のバージョン」と書いていますが、`AppMain.exe` に入るファイルにも、ライセンスの表記が無いもの(`memmap.c`、`gba_cc_lut.c`、`bios_data.S`、`gpsp_config.h`、`gba_over.h`、`libretro/libretro.c`、`libretro/libretro_core_options.h` など)があり、上流の README にもライセンスの記載はありません。

条件は次のとおりです。

- ソース・バイナリとも再配布できます。GPL の範囲で商用利用もできます
- 配るときは、ライセンスの文と各ファイルの著作権表示を残すこと
- `AppMain.exe` を配布するときは、対応するソース(このリポジトリ)も入手できるようにすること
- 改変したものを配るときは、全体を GPL で配布すること
- 保証はありません

非営利に限る条項を持つものは含まれていません。

## 2. gpSP 本体(上流のファイル)

リポジトリ直下のファイルは、上流の [libretro/gpsp](https://github.com/libretro/gpsp) のコミット `5819380`(2026-09-19)を元にしています。gpSP は Exophase 氏の作で、libretro 版は David Guillen Fandos 氏と libretro のコントリビューターが保守しています。

`AppMain.exe` に入る gpSP のファイルは、`CE/Makefile` の `SOURCES_CC` と `SOURCES_CORE` にあるものです。

PopGBA が変えた上流のファイルは次の2つだけです。どれも Windows CE 用のツール(cegcc)とこの端末に合わせるためのもので、ライセンスは変わりません。

- `arm/arm_stub.S`: cegcc のアセンブラが読める書き方に直した(ELF 用の `.type` / `.size` を外し、翻訳キャッシュを通常の `.bss` に置いた)
- `cpu_threaded.c`: `platform_cache_sync()` に Windows CE 用の処理を足した(`CacheSync(CACHE_SYNC_ALL)` を呼ぶ。このツールの `__clear_cache()` は何もしないため)

上流のソースの著作権表示は、変えずに残しています。

## 3. PopGBA の自作部分: MIT

PopGBA の自作部分は、MIT ライセンスです。本文は [`LICENSE`](LICENSE) の2節にあります。対象は、先頭に `SPDX-License-Identifier: MIT` と書いてある次のファイルです。

- `CE/` のフロントエンド(`ce_*.c`、`ce_*.h`、`ce_res.rc`、`compat/`、`Makefile`)。ただし次のものは除きます
  - フォントのデータ `ce_shinonome16.h`、`ce_galmuri14.h`、`ce_galmuri11.h`(下の「第三者のもの」を参照)
  - マスコットの画像とアプリのアイコン(下の「マスコットの絵とアイコン」を参照)

自作部分だけを取り出して、ほかのプロジェクトで MIT として使うことができます。gpSP と組み合わせて配布する場合は、1 の条件も守る必要があります。

## 4. マスコットの絵とアイコン: CC0 1.0

メニューのマスコットの絵(`CE/icon/popgba_mascot.bmp`)、アプリのアイコン(`CE/icon/popgba.ico`、`AppMain.exe` に入っているもの)、README の先頭の絵(`.github/images/popgba_mascot_C_osanpo_4x.png`)は、[CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/)(パブリックドメイン)です。アイコンは、Pop シリーズのマスコットをもとに AI(Claude)で作りました。

## 5. 第三者のもの

`AppMain.exe` に入っている第三者のものは次のとおりです。著作権表示と許諾文は [`THIRDPARTY_LICENSES.txt`](THIRDPARTY_LICENSES.txt) にまとめています。

| もの | 作者 | ライセンス |
|---|---|---|
| オープンソースの GBA BIOS(`bios/open_gba_bios.bin`) | Normmatt 氏、VBA / VBA-M 開発チーム | GPL-2.0 |
| ARM 命令のマクロ(`arm/arm_codegen.h`、`arm/arm_dpimacros.h`) | Sergey Chaban 氏、Wild West Software | MIT |
| libretro API のヘッダ、libretro-common の一部 | The RetroArch team | MIT |
| Galmuri フォント(メニューの既定の文字) | Lee Minseo 氏 | SIL Open Font License 1.1 |
| 東雲 16 ドットフォント | 古川泰之 氏ほか、/efont/ | 実質パブリックドメイン |

オープンソースの BIOS は、任天堂の公式 BIOS の代わりとなるものです。`gba_bios.bin` が見つからないときや、中身が正しくないときに使います。

## 6. 同梱していないもの

- ゲームの ROM
- GBA 本体の BIOS(`gba_bios.bin`)。任天堂の著作物なので、使う人が自分で用意してください

## 7. 上流のファイルについての注意

リポジトリ直下の `3ds/`、`jni/`、`mips/`、`x86/`、`tests/`、`tools/`、`README.md`、`original_readme.txt` などは上流のファイルです。PopGBA のビルドには使っていません(`AppMain.exe` には入っていません)。
