# ルネサス公式サンプルのロードモジュール（テスト用）

出所: https://github.com/renesas/rl78-sc-examples （コミット cd92b99726fbc5187d7d9a61f4a351646a5fff7b、2026-10-28）。
ライセンスは BSD-3-Clause（同じフォルダの `LICENSE`。Copyright (c) 2026, Renesas Electronics）。

CS+ / CC-RL V1.12.00 でビルドされたものを、そのまま（再ビルドせず）置いている。

| フォルダ | 元のパス（example_projects/rl78_g2x/ 以下） | 品種 |
|---|---|---|
| `g22_adc/` | `rl78_g22/adc_software_trigger_sequential_conversion_modes/cs+/r01an6640-rl78g22-adc/DefaultBuild/` | RL78/G22（S3） |
| `g23_spi/` | `rl78_g23/handshake_based_spi_slave_transmission_reception/cs+/woekspace/DefaultBuild/` | RL78/G23（S3） |

`.abs`（ELF + DWARF 2/3）と `.map` のほか、テスト CSV の期待値の根拠になる C ソースとスタートアップだけを `src/` に写した
（各ファイル先頭のルネサスの注記はそのまま）。期待値の導き方は `tests/rl78/csv/*.csv` のコメント行に書いてある。
