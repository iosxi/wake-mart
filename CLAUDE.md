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
  （`sleep-guard` と同じ理由。利用者が作ったショートカットが exe のパスを指している）。
- `FileVersion` は `wakemart.rc` の VERSIONINFO（v3 時点で `1.2.0`）と manifest の version。
  タグの `vN` とは連動させていない。機能が変わったら上げる。

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
- **タイマーで起きたかは、時刻ではなく Power-Troubleshooter（ID 1）の記録で判定する。**
  タイマーなら `WakeTimerOwner` に exe のパスが入る。`WakeSourceType` はタイマーでも 8 の回と
  6 の回があった（v3 で、種別でなく `WakeTimerOwner` で判定するよう直した）。
  5＝機器（`WakeSourceText` に機器名）、0＝不明。
  Windows 10 の利用者の環境では、予定どおり +8 秒で起きても 0（不明）と記録された。
  2026-10-04 の試験では、タイマー時刻とほぼ一致した復帰でも 0（不明）が何度もあり、
  時刻だけで「成功」とすると誤る（「スリープして試す」は v2 でこの方式に直した）。
- **この PC は、数十秒先のタイマーを抱えて寝かせると、十数秒で原因不明（種別 0）で起きる。**
  8〜32 秒先で 6 回中 6 回（休止状態でも 1 回）。タイマー許可を「無効」にしても同じだった。60 秒・91 秒先なら
  種別 8 で予定どおり起きた。復帰の試験は **60 秒以上先**に置く。
- 利用者は試験時間を短くしてほしいと言っている（2026-10-04）。必要な回数だけ、
  60 秒程度で行う。画面が消えている間は触らないよう頼む（触って起こしたことがある）。
- 「スリープ解除タイマーの許可」（RTCWAKE）は一般権限で変えられる:
  `powercfg /setacvalueindex SCHEME_CURRENT SUB_SLEEP RTCWAKE 0|1` → `powercfg /setactive SCHEME_CURRENT`。
  **この PC の元の値は AC=1（有効）、DC=0（無効）。試験で変えたら必ず戻す。**
- RTCWAKE＝無効のとき、アプリが `SetSuspendState` で寝かせてもタイマーでは起きない（Windows 11 で確認）。
  Windows 10 で「別のツール（mt_Power_off）なら無効でも起きた」という報告があったが、
  そのツールも `CreateWaitableTimer`＋`SetWaitableTimer`＋`SetSuspendState` だけで、設定を
  書き換える処理は入っていない。Windows 10 では確かめていない。
