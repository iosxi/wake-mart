# wake-mart の作業方針

共通の方針は `C:\projects\windows\CLAUDE.md`（C ＋ MSVC など）と `~/.claude/CLAUDE.md` にある。
ここにはこのリポジトリ固有の事情だけを書く。

## リリース運用

**手順は共通の `~/.claude/CLAUDE.md`「修正が終わったら、リリースまで通す」に従う。**

- リモート: `https://github.com/iosxi/wake-mart.git`（`iosxi/wake-mart`）
- ブランチ: **`master`**（`main` ではない）
- 最新バージョンの確認: `git tag --sort=-v:refname | head -1`
- exe（`wake-mart.exe`）はリポジトリに追跡させている。ソースを直したら
  `build.cmd` で作り直してからコミットする。
- リリースの添付物にするときは `wake-mart.exe` を**改名せずにそのまま**渡す
  （`sleep-guard` と同じ理由。スタートアップのショートカットが exe のパスを指している）。
- `FileVersion` は `wakemart.rc` の VERSIONINFO（v1 時点で `1.0.0`）。タグの `vN` とは連動させていない。

## 動作確認について

- **利用者の本番設定を使わない。** 確認はスクラッチ領域にテスト用 ini を作り、
  `-tray -config <ini>` で起動、`-exit -config <ini>` で終了させる。設定ファイルごとに
  別インスタンスになるので、利用者が動かしている wake-mart とぶつからない。
- 結果は `<ini と同じ名前>.log` で読む。鳴った時刻・遅れ秒数・スリープ／復帰・
  画面のオン／オフ・ウェイクタイマーの設定時刻が出る。画面を撮らずに大半を確かめられる。
- 画面のレイアウトは、対象ウィンドウだけを `PrintWindow` で PNG にして見る
  （前面化もキー操作もしない）。撮る側の PowerShell は `SetProcessDPIAware()` しないと
  この PC（150%）では右下が切れる。トレイ起動だと `MainWindowHandle` が鳴動窓を指すので、
  窓はクラス名 `WakeMartMain` で区別する。
- 窓を閉じるのは `PostMessage(WM_COMMAND, IDOK/IDCANCEL)`。

### スリープ・休止状態からの復帰を確かめるとき

- アラーム A（`Power=1` スリープ / `2` 休止）で寝かせ、アラーム B（`Wake=1`）で起こす。
  A は鳴ってから 30 秒の確認ののちに寝る。B の起床は予定の 30 秒前。
- **この PC はタイマーと無関係に早く起きることがある**（2026-10-04 の試験で、S3 で 10.5 秒後・
  S4 で 33 秒後に `Wake Source: Unknown`。利用者は触っていない。原因は不明）。
  タイマー無しでは 3 分以上眠ったままだった回もあるので、常にではない。
- タイマーで起きたことは System ログの Power-Troubleshooter（ID 1）で確かめる。
  `Wake Source: Unknown, but possibily due to timer - wake-mart.exe` と出れば wake-mart のタイマー。
  実際に眠っていた秒数は Kernel-General（ID 1）の `TimeDeltaInMs`、S3/S4 の別は
  Kernel-Power（ID 42）の `TargetState`（4=S3、5=S4）で分かる。
- `SetSuspendState` は復帰するまで UI スレッドを止めるので、`PBT_APMSUSPEND` のログは
  復帰後の時刻で残る。
