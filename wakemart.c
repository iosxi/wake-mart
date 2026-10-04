// wake-mart: 指定した日時にメッセージ・音・プログラム実行・電源操作を行う常駐アラーム
//
// スリープ（S3）や休止状態（S4）に入っていても、SetWaitableTimer のスリープ解除
// タイマー（fResume=TRUE）でマシンを起こしてから実行する。タスクスケジューラも
// レジストリも使わない（設定は exe の隣の wake-mart.ini）。自動起動の仕組みは持たない
// （v3 で利用者の要望により外した）。
//
// 鳴らす判定は 0.5 秒ごとのタイマーで行い、ウェイクタイマーは「起こす」ためだけに
// 使う。起こすのは予定の WAKE_LEAD_SEC 秒前（休止状態からの復帰にかかる時間の分）。
//
// コマンドライン:
//   wake-mart.exe                起動（既に動いていればその画面を出す）
//   wake-mart.exe -tray          画面を出さずにタスクトレイで起動
//   wake-mart.exe -exit          動いている wake-mart を終了させる
//   wake-mart.exe -config <ini>  設定ファイルを指定（既定は exe の隣の wake-mart.ini）
#define _CRT_SECURE_NO_WARNINGS
#define _WIN32_WINNT 0x0601        // Windows 10 より新しい API を使わないための目安
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <commdlg.h>
#include <mmsystem.h>
#include <powrprof.h>
#include <wchar.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "resource.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "powrprof.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")

#define APP_NAME        L"wake-mart"
#define MAIN_CLASS      L"WakeMartMain"
#define PROP_KEY        L"WakeMartKey"
#define WM_TRAY         (WM_APP + 1)
#define WM_APP_CMD      (WM_APP + 2)      // 別インスタンスからの指示（wParam: CMD_*）
#define CMD_SHOW        1
#define CMD_EXIT        2
#define TIMER_TICK      1
#define TICK_MS         500
#define MAX_ALARMS      200
#define MAX_SNOOZES     32
#define MAX_JOBS        32
#define MAX_POPUPS      32
#define WAKE_LEAD_SEC   30                // 予定の何秒前に起こすか
#define LATE_GRACE_SEC  180               // これ以上遅れたら「取りこぼし」扱い
#define AWAKE_AFTER_SEC 30                // 鳴らしたあとスリープさせない秒数
#define COUNTDOWN_SEC   30                // 電源操作の前の確認
#define SOUND_MAX_SEC   600               // 音は最長 10 分で止める
#define TPS             10000000ULL       // FILETIME の 1 秒

enum { K_ONCE, K_DAILY, K_WEEKLY, K_MONTHLY, K_INTERVAL, K_COUNT };
enum { PW_NONE, PW_SLEEP, PW_HIBERNATE, PW_SHUTDOWN, PW_REBOOT, PW_LOGOFF, PW_MONITOR, PW_COUNT };

static const wchar_t *KIND_NAMES[K_COUNT] = {
    L"1回だけ", L"毎日", L"毎週（曜日指定）", L"毎月（日付指定）", L"一定間隔" };
static const wchar_t *POWER_NAMES[PW_COUNT] = {
    L"なし", L"スリープ", L"休止状態", L"シャットダウン", L"再起動", L"サインアウト", L"画面オフ" };
static const wchar_t *SHOW_NAMES[3] = { L"通常", L"最小化", L"非表示" };
static const wchar_t *WDAY[7] = { L"日", L"月", L"火", L"水", L"木", L"金", L"土" };

// 電源設定「スリープ解除タイマーの許可」（RTCWAKE）
static const GUID GUID_SLEEP_SUB  = { 0x238c9fa8, 0x0aad, 0x41ed, { 0x83, 0xf4, 0x97, 0xbe, 0x24, 0x2c, 0x8f, 0x20 } };
static const GUID GUID_RTCWAKE    = { 0xbd3b718a, 0x0680, 0x4d9d, { 0x8a, 0xb2, 0xe1, 0xd2, 0xb4, 0xac, 0x80, 0x6d } };
static const GUID GUID_ACDC       = { 0x5d3e9a59, 0xe9d5, 0x4b00, { 0xa6, 0xbd, 0xff, 0x34, 0xff, 0x51, 0x65, 0x48 } };
static const GUID GUID_DISPLAY    = { 0x6fe69556, 0x704a, 0x47a0, { 0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47 } };

typedef struct {
    int id;
    BOOL enabled;
    wchar_t name[128];
    int kind;
    SYSTEMTIME start;       // ローカル時刻。1回/一定間隔は日付も使い、それ以外は時刻だけ使う
    int weekdays;           // bit0=日 … bit6=土
    int mday;               // 1〜31（その月にない日は月末）
    int interval;           // 分
    BOOL actMsg;   wchar_t msg[1024];
    BOOL actSound; wchar_t sound[MAX_PATH];
    BOOL actCmd;   wchar_t cmd[MAX_PATH]; wchar_t args[1024]; wchar_t dir[MAX_PATH];
    int show;               // 0=通常 1=最小化 2=非表示
    BOOL cmdWait;
    int power;
    BOOL wake, display, catchup;
    int snooze;             // 分。0 ならスヌーズなし
    ULONGLONG next;         // 次回（UTC の FILETIME 値）。0 は予定なし。保存しない
} Alarm;

typedef struct { int id; ULONGLONG due; } Snooze;
typedef struct { HANDLE proc; int power; wchar_t name[128]; } Job;

static HINSTANCE g_inst;
static HWND      g_main, g_list;
static HICON     g_iconLarge, g_iconSmall;
static NOTIFYICONDATAW g_nid;
static UINT      g_msgTaskbarCreated;
static wchar_t   g_exePath[MAX_PATH], g_iniPath[MAX_PATH], g_logPath[MAX_PATH];
static BOOL      g_customIni;
static UINT      g_key;                   // 設定ファイルごとのインスタンス識別

static Alarm     g_alarms[MAX_ALARMS];
static int       g_count, g_nextId = 1;
static Snooze    g_snoozes[MAX_SNOOZES];
static int       g_snoozeCount;
static Job       g_jobs[MAX_JOBS];
static int       g_jobCount;
static HWND      g_popups[MAX_POPUPS];
static int       g_popupCount;

static HANDLE    g_wakeTimer;
static ULONGLONG g_wakeArmed, g_wakeDue;
static int       g_wakeId;
static EXECUTION_STATE g_esApplied;
static ULONGLONG g_lastFireTick;
static HWND      g_countdown;
static int       g_cdOp, g_cdLeft;
static wchar_t   g_cdName[128];
static BOOL      g_hideNoticeShown, g_listUpdating;

// ---- 時刻 ----

static ULONGLONG ft_to_u64(FILETIME f) { return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; }
static FILETIME u64_to_ft(ULONGLONG t) { FILETIME f = { (DWORD)t, (DWORD)(t >> 32) }; return f; }

static ULONGLONG now_utc(void) {
    FILETIME f; GetSystemTimeAsFileTime(&f);
    return ft_to_u64(f);
}

static ULONGLONG st_to_u64(const SYSTEMTIME *st) {
    FILETIME f;
    if (!SystemTimeToFileTime(st, &f)) return 0;
    return ft_to_u64(f);
}

static void u64_to_st(ULONGLONG t, SYSTEMTIME *st) {
    FILETIME f = u64_to_ft(t);
    FileTimeToSystemTime(&f, st);
}

static ULONGLONG local_to_utc(const SYSTEMTIME *local) {
    SYSTEMTIME u;
    if (!TzSpecificLocalTimeToSystemTime(NULL, local, &u)) return 0;
    return st_to_u64(&u);
}

static void utc_to_local(ULONGLONG t, SYSTEMTIME *local) {
    SYSTEMTIME u; u64_to_st(t, &u);
    SystemTimeToTzSpecificLocalTime(NULL, &u, local);
}

// ローカルの日付に日数を足す（時刻はそのまま。曜日も入れ直される）
static SYSTEMTIME add_days(const SYSTEMTIME *st, int days) {
    SYSTEMTIME r;
    u64_to_st(st_to_u64(st) + (ULONGLONG)days * 86400ULL * TPS, &r);
    return r;
}

static int days_in_month(int y, int m) {
    static const int d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return d[m - 1];
}

// after より後で最初の予定（UTC）。無ければ 0
static ULONGLONG next_after(const Alarm *a, ULONGLONG after) {
    SYSTEMTIME nowL, base;
    utc_to_local(after, &nowL);
    base = nowL;
    base.wHour = a->start.wHour; base.wMinute = a->start.wMinute;
    base.wSecond = a->start.wSecond; base.wMilliseconds = 0;

    switch (a->kind) {
    case K_ONCE: {
        SYSTEMTIME s = a->start; s.wMilliseconds = 0;
        ULONGLONG t = local_to_utc(&s);
        return t > after ? t : 0;
    }
    case K_DAILY:
        for (int d = 0; d <= 2; d++) {
            SYSTEMTIME s = add_days(&base, d);
            ULONGLONG t = local_to_utc(&s);
            if (t > after) return t;
        }
        return 0;
    case K_WEEKLY:
        if (!(a->weekdays & 0x7f)) return 0;
        for (int d = 0; d <= 8; d++) {
            SYSTEMTIME s = add_days(&base, d);
            if (!(a->weekdays >> s.wDayOfWeek & 1)) continue;
            ULONGLONG t = local_to_utc(&s);
            if (t > after) return t;
        }
        return 0;
    case K_MONTHLY: {
        int y = nowL.wYear, m = nowL.wMonth;
        for (int i = 0; i < 14; i++) {
            int dim = days_in_month(y, m);
            SYSTEMTIME s = base;
            s.wYear = (WORD)y; s.wMonth = (WORD)m;
            s.wDay = (WORD)(a->mday < 1 ? 1 : a->mday > dim ? dim : a->mday);
            ULONGLONG t = local_to_utc(&s);
            if (t > after) return t;
            if (++m > 12) { m = 1; y++; }
        }
        return 0;
    }
    case K_INTERVAL: {
        if (a->interval < 1) return 0;
        SYSTEMTIME s = a->start; s.wMilliseconds = 0;
        ULONGLONG b = local_to_utc(&s), step = (ULONGLONG)a->interval * 60ULL * TPS;
        if (!b) return 0;
        if (after < b) return b;
        return b + ((after - b) / step + 1) * step;
    }
    }
    return 0;
}

// ---- 表示用の文字列 ----

static void fmt_local(ULONGLONG utc, wchar_t *b, size_t n) {
    SYSTEMTIME s; utc_to_local(utc, &s);
    swprintf(b, n, L"%04d/%02d/%02d(%ls) %02d:%02d:%02d",
             s.wYear, s.wMonth, s.wDay, WDAY[s.wDayOfWeek], s.wHour, s.wMinute, s.wSecond);
}

static void fmt_weekdays(int w, wchar_t *b, size_t n) {
    w &= 0x7f;
    if (w == 0x7f)      wcsncpy(b, L"毎日", n);
    else if (w == 0x3e) wcsncpy(b, L"平日", n);
    else if (w == 0x41) wcsncpy(b, L"土日", n);
    else {
        b[0] = 0;
        for (int i = 0; i < 7; i++) if (w >> i & 1) wcsncat(b, WDAY[i], n - wcslen(b) - 1);
    }
}

static void describe_schedule(const Alarm *a, wchar_t *b, size_t n) {
    const SYSTEMTIME *s = &a->start;
    wchar_t t[16], w[32];
    swprintf(t, 16, L"%02d:%02d:%02d", s->wHour, s->wMinute, s->wSecond);
    switch (a->kind) {
    case K_ONCE:    swprintf(b, n, L"1回 %04d/%02d/%02d %ls", s->wYear, s->wMonth, s->wDay, t); break;
    case K_DAILY:   swprintf(b, n, L"毎日 %ls", t); break;
    case K_WEEKLY:  fmt_weekdays(a->weekdays, w, 32); swprintf(b, n, L"毎週 %ls %ls", w, t); break;
    case K_MONTHLY: swprintf(b, n, L"毎月 %d日 %ls", a->mday, t); break;
    case K_INTERVAL:
        if (a->interval % 60 == 0) swprintf(b, n, L"%d時間ごと（%04d/%02d/%02d %ls から）",
                                            a->interval / 60, s->wYear, s->wMonth, s->wDay, t);
        else                       swprintf(b, n, L"%d分ごと（%04d/%02d/%02d %ls から）",
                                            a->interval, s->wYear, s->wMonth, s->wDay, t);
        break;
    default: b[0] = 0;
    }
}

static void describe_actions(const Alarm *a, wchar_t *b, size_t n) {
    b[0] = 0;
    if (a->actMsg)   wcsncat(b, L"メッセージ・", n - wcslen(b) - 1);
    if (a->actSound) wcsncat(b, L"音・", n - wcslen(b) - 1);
    if (a->actCmd) {
        const wchar_t *f = wcsrchr(a->cmd, L'\\');
        wchar_t s[MAX_PATH + 8];
        swprintf(s, MAX_PATH + 8, L"実行 %ls・", f ? f + 1 : a->cmd);
        wcsncat(b, s, n - wcslen(b) - 1);
    }
    size_t len = wcslen(b);
    if (len) b[len - 1] = 0;              // 最後の「・」を落とす
    if (a->power) {
        wchar_t s[32];
        swprintf(s, 32, L"%ls→%ls", b[0] ? L" " : L"", POWER_NAMES[a->power]);
        wcsncat(b, s, n - wcslen(b) - 1);
    }
}

// ---- ログ（設定ファイルの隣の wake-mart.log） ----

static void logw(const wchar_t *fmt, ...) {
    wchar_t msg[1024], line[1200];
    va_list ap; va_start(ap, fmt);
    vswprintf(msg, ARRAYSIZE(msg), fmt, ap);
    va_end(ap);
    SYSTEMTIME s; GetLocalTime(&s);
    int n = swprintf(line, ARRAYSIZE(line), L"%04d-%02d-%02d %02d:%02d:%02d.%03d %ls\r\n",
                     s.wYear, s.wMonth, s.wDay, s.wHour, s.wMinute, s.wSecond, s.wMilliseconds, msg);
    if (n <= 0) return;

    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(g_logPath, GetFileExInfoStandard, &fa) && fa.nFileSizeLow > 512 * 1024) {
        wchar_t old[MAX_PATH + 8];
        swprintf(old, ARRAYSIZE(old), L"%ls.old", g_logPath);
        MoveFileExW(g_logPath, old, MOVEFILE_REPLACE_EXISTING);
    }
    char u8[3600];
    int len = WideCharToMultiByte(CP_UTF8, 0, line, n, u8, sizeof u8, NULL, NULL);
    HANDLE h = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wr; WriteFile(h, u8, (DWORD)len, &wr, NULL);
    CloseHandle(h);
}

// ---- 設定ファイル（UTF-16 の INI。レジストリは使わない） ----
//
// 値は必ず "…" で囲んで書く（Profile API は両端の引用符を外すので、値の中の引用符が
// 壊れない）。改行・タブ・\ はエスケープする。

static void ini_escape(const wchar_t *s, wchar_t *out, size_t n) {
    size_t o = 0;
    out[o++] = L'"';
    for (; *s && o + 3 < n; s++) {
        if (*s == L'\\')      { out[o++] = L'\\'; out[o++] = L'\\'; }
        else if (*s == L'\n') { out[o++] = L'\\'; out[o++] = L'n'; }
        else if (*s == L'\t') { out[o++] = L'\\'; out[o++] = L't'; }
        else if (*s == L'\r') { }
        else out[o++] = *s;
    }
    out[o++] = L'"';
    out[o] = 0;
}

static void ini_unescape(const wchar_t *s, wchar_t *out, size_t n) {
    size_t o = 0;
    for (; *s && o + 2 < n; s++) {
        if (*s == L'\\' && s[1]) {
            s++;
            if (*s == L'n')      { out[o++] = L'\r'; out[o++] = L'\n'; }
            else if (*s == L't') out[o++] = L'\t';
            else                 out[o++] = *s;
        } else out[o++] = *s;
    }
    out[o] = 0;
}

static void ini_put(const wchar_t *file, const wchar_t *sec, const wchar_t *key, const wchar_t *val) {
    static wchar_t buf[4200];
    ini_escape(val, buf, ARRAYSIZE(buf));
    WritePrivateProfileStringW(sec, key, buf, file);
}

static void ini_put_int(const wchar_t *file, const wchar_t *sec, const wchar_t *key, int v) {
    wchar_t s[16]; swprintf(s, 16, L"%d", v);
    WritePrivateProfileStringW(sec, key, s, file);
}

static void ini_get(const wchar_t *sec, const wchar_t *key, wchar_t *out, size_t n) {
    static wchar_t buf[4200];
    GetPrivateProfileStringW(sec, key, L"", buf, ARRAYSIZE(buf), g_iniPath);
    ini_unescape(buf, out, n);
}

static int ini_get_int(const wchar_t *sec, const wchar_t *key, int def) {
    return (int)GetPrivateProfileIntW(sec, key, def, g_iniPath);
}

static void save_alarms(void) {
    wchar_t tmp[MAX_PATH + 8];
    swprintf(tmp, ARRAYSIZE(tmp), L"%ls.tmp", g_iniPath);
    // BOM 付き UTF-16 の空ファイルを先に作ると、Profile API が Unicode で書く
    HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { logw(L"設定を保存できません（%ls, err=%lu）", tmp, GetLastError()); return; }
    static const BYTE bom[2] = { 0xff, 0xfe };
    DWORD wr; WriteFile(h, bom, 2, &wr, NULL);
    CloseHandle(h);

    ini_put_int(tmp, L"Settings", L"Count", g_count);
    ini_put_int(tmp, L"Settings", L"NextId", g_nextId);
    for (int i = 0; i < g_count; i++) {
        const Alarm *a = &g_alarms[i];
        wchar_t sec[32], t[32];
        swprintf(sec, 32, L"Alarm%d", i + 1);
        swprintf(t, 32, L"%04d-%02d-%02d %02d:%02d:%02d", a->start.wYear, a->start.wMonth, a->start.wDay,
                 a->start.wHour, a->start.wMinute, a->start.wSecond);
        ini_put_int(tmp, sec, L"Id", a->id);
        ini_put_int(tmp, sec, L"Enabled", a->enabled);
        ini_put(tmp, sec, L"Name", a->name);
        ini_put_int(tmp, sec, L"Kind", a->kind);
        ini_put(tmp, sec, L"Start", t);
        ini_put_int(tmp, sec, L"Weekdays", a->weekdays);
        ini_put_int(tmp, sec, L"MonthDay", a->mday);
        ini_put_int(tmp, sec, L"Interval", a->interval);
        ini_put_int(tmp, sec, L"Message", a->actMsg);
        ini_put(tmp, sec, L"MessageText", a->msg);
        ini_put_int(tmp, sec, L"Sound", a->actSound);
        ini_put(tmp, sec, L"SoundFile", a->sound);
        ini_put_int(tmp, sec, L"Run", a->actCmd);
        ini_put(tmp, sec, L"RunFile", a->cmd);
        ini_put(tmp, sec, L"RunArgs", a->args);
        ini_put(tmp, sec, L"RunDir", a->dir);
        ini_put_int(tmp, sec, L"RunShow", a->show);
        ini_put_int(tmp, sec, L"RunWait", a->cmdWait);
        ini_put_int(tmp, sec, L"Power", a->power);
        ini_put_int(tmp, sec, L"Wake", a->wake);
        ini_put_int(tmp, sec, L"DisplayOn", a->display);
        ini_put_int(tmp, sec, L"CatchUp", a->catchup);
        ini_put_int(tmp, sec, L"Snooze", a->snooze);
    }
    WritePrivateProfileStringW(NULL, NULL, NULL, tmp);   // 書き出しを確定させる
    if (!MoveFileExW(tmp, g_iniPath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        logw(L"設定を保存できません（err=%lu）", GetLastError());
}

static void default_alarm(Alarm *a) {
    ZeroMemory(a, sizeof *a);
    a->enabled = TRUE;
    wcscpy(a->name, L"アラーム");
    a->kind = K_ONCE;
    SYSTEMTIME now; GetLocalTime(&now);
    now.wSecond = 0; now.wMilliseconds = 0;
    u64_to_st(st_to_u64(&now) + 10ULL * 60ULL * TPS, &a->start);   // 10 分後
    a->weekdays = 0x3e;
    a->mday = 1;
    a->interval = 60;
    a->actMsg = TRUE;
    a->actSound = TRUE;
    a->wake = TRUE;
    a->display = TRUE;
    a->snooze = 5;
}

static void load_alarms(void) {
    g_count = 0;
    int n = ini_get_int(L"Settings", L"Count", 0);
    g_nextId = ini_get_int(L"Settings", L"NextId", 1);
    for (int i = 0; i < n && g_count < MAX_ALARMS; i++) {
        Alarm *a = &g_alarms[g_count];
        wchar_t sec[32], t[64];
        swprintf(sec, 32, L"Alarm%d", i + 1);
        default_alarm(a);
        a->id = ini_get_int(sec, L"Id", 0);
        if (a->id <= 0) continue;
        a->enabled = ini_get_int(sec, L"Enabled", 1);
        ini_get(sec, L"Name", a->name, ARRAYSIZE(a->name));
        a->kind = ini_get_int(sec, L"Kind", K_ONCE);
        if (a->kind < 0 || a->kind >= K_COUNT) a->kind = K_ONCE;
        ini_get(sec, L"Start", t, ARRAYSIZE(t));
        int y, mo, d, h, mi, s;
        if (swscanf(t, L"%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) == 6) {
            ZeroMemory(&a->start, sizeof a->start);
            a->start.wYear = (WORD)y; a->start.wMonth = (WORD)mo; a->start.wDay = (WORD)d;
            a->start.wHour = (WORD)h; a->start.wMinute = (WORD)mi; a->start.wSecond = (WORD)s;
            u64_to_st(st_to_u64(&a->start), &a->start);   // 曜日を入れ直す
        }
        a->weekdays = ini_get_int(sec, L"Weekdays", 0x3e);
        a->mday = ini_get_int(sec, L"MonthDay", 1);
        a->interval = ini_get_int(sec, L"Interval", 60);
        a->actMsg = ini_get_int(sec, L"Message", 0);
        ini_get(sec, L"MessageText", a->msg, ARRAYSIZE(a->msg));
        a->actSound = ini_get_int(sec, L"Sound", 0);
        ini_get(sec, L"SoundFile", a->sound, ARRAYSIZE(a->sound));
        a->actCmd = ini_get_int(sec, L"Run", 0);
        ini_get(sec, L"RunFile", a->cmd, ARRAYSIZE(a->cmd));
        ini_get(sec, L"RunArgs", a->args, ARRAYSIZE(a->args));
        ini_get(sec, L"RunDir", a->dir, ARRAYSIZE(a->dir));
        a->show = ini_get_int(sec, L"RunShow", 0);
        if (a->show < 0 || a->show > 2) a->show = 0;
        a->cmdWait = ini_get_int(sec, L"RunWait", 0);
        a->power = ini_get_int(sec, L"Power", 0);
        if (a->power < 0 || a->power >= PW_COUNT) a->power = PW_NONE;
        a->wake = ini_get_int(sec, L"Wake", 1);
        a->display = ini_get_int(sec, L"DisplayOn", 1);
        a->catchup = ini_get_int(sec, L"CatchUp", 0);
        a->snooze = ini_get_int(sec, L"Snooze", 5);
        if (a->id >= g_nextId) g_nextId = a->id + 1;
        g_count++;
    }
}

static Alarm *find_alarm(int id) {
    for (int i = 0; i < g_count; i++) if (g_alarms[i].id == id) return &g_alarms[i];
    return NULL;
}

static void recompute_next(Alarm *a) {
    a->next = a->enabled ? next_after(a, now_utc()) : 0;
}

// ---- 実行状態（スリープさせない・画面を点ける） ----

static void wake_display(void) {
    SetThreadExecutionState(ES_DISPLAY_REQUIRED);    // 画面のアイドルタイマーを戻す
    // タイマーで起きた直後は「無人」扱いで画面が消えたままのことがあるので、
    // 動かない程度のマウス入力も送って利用者が戻った扱いにする
    INPUT in[2];
    ZeroMemory(in, sizeof in);
    in[0].type = INPUT_MOUSE; in[0].mi.dx = 1;  in[0].mi.dwFlags = MOUSEEVENTF_MOVE;
    in[1].type = INPUT_MOUSE; in[1].mi.dx = -1; in[1].mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(2, in, sizeof(INPUT));
}

static int popups_wanting_display(void);

static void update_exec_state(void) {
    ULONGLONG now = now_utc();
    BOOL sys = g_popupCount > 0 || g_countdown != NULL || g_jobCount > 0
            || GetTickCount64() - g_lastFireTick < AWAKE_AFTER_SEC * 1000ULL;
    // 起こした直後から予定までの間（と予定直前）は寝かせない
    if (g_wakeDue && g_wakeDue > now && g_wakeDue - now <= (WAKE_LEAD_SEC + 60) * TPS) sys = TRUE;

    EXECUTION_STATE f = ES_CONTINUOUS;
    if (sys) f |= ES_SYSTEM_REQUIRED;
    if (popups_wanting_display() > 0) f |= ES_DISPLAY_REQUIRED;
    if (f != g_esApplied) {
        SetThreadExecutionState(f);
        g_esApplied = f;
    }
}

// ---- ウェイクタイマー ----

static void update_status(void);

static void arm_wake(BOOL force) {
    ULONGLONG now = now_utc(), due = 0;
    int id = 0;
    for (int i = 0; i < g_count; i++) {
        const Alarm *a = &g_alarms[i];
        if (a->enabled && a->wake && a->next && (!due || a->next < due)) { due = a->next; id = a->id; }
    }
    for (int i = 0; i < g_snoozeCount; i++) {
        const Alarm *a = find_alarm(g_snoozes[i].id);
        if (a && a->wake && (!due || g_snoozes[i].due < due)) { due = g_snoozes[i].due; id = a->id; }
    }
    ULONGLONG at = 0;
    if (due) {
        at = due - WAKE_LEAD_SEC * TPS;
        if (at <= now + 2 * TPS) at = due;   // もう前倒しの時刻を過ぎていたら予定そのもので
        if (at <= now) at = 0;               // 予定自体が今なので、起こす必要はない
    }
    if (!force && at == g_wakeArmed && due == g_wakeDue) return;
    g_wakeArmed = at; g_wakeDue = due; g_wakeId = id;
    if (!at) {
        CancelWaitableTimer(g_wakeTimer);
    } else {
        LARGE_INTEGER li; li.QuadPart = (LONGLONG)at;   // 正の値は UTC の絶対時刻
        SetLastError(0);
        if (!SetWaitableTimer(g_wakeTimer, &li, 0, NULL, NULL, TRUE))
            logw(L"ウェイクタイマーを設定できません（err=%lu）", GetLastError());
        else if (GetLastError() == ERROR_NOT_SUPPORTED)
            logw(L"この PC はタイマーによるスリープ解除に対応していません");
        else {
            wchar_t s[64]; fmt_local(at, s, 64);
            logw(L"ウェイクタイマー設定: %ls（アラーム id=%d）", s, id);
        }
    }
    update_status();
}

// 今の電源（AC/バッテリー）での「スリープ解除タイマーの許可」。-1 は読めない
static int read_rtcwake(BOOL *onBattery) {
    SYSTEM_POWER_STATUS ps;
    *onBattery = GetSystemPowerStatus(&ps) && ps.ACLineStatus == 0;
    GUID *scheme = NULL;
    if (PowerGetActiveScheme(NULL, &scheme) != ERROR_SUCCESS) return -1;
    DWORD v = 1, r;
    r = *onBattery ? PowerReadDCValueIndex(NULL, scheme, &GUID_SLEEP_SUB, &GUID_RTCWAKE, &v)
                   : PowerReadACValueIndex(NULL, scheme, &GUID_SLEEP_SUB, &GUID_RTCWAKE, &v);
    LocalFree(scheme);
    return r == ERROR_SUCCESS ? (int)v : -1;
}

// 今の電源（AC/バッテリー）の「スリープ解除タイマーの許可」を「有効」にする。
// 電源プランは一般権限でも書き換えられる（powercfg と同じ API）。戻り値は Win32 エラー
static DWORD enable_rtcwake(BOOL onBattery) {
    GUID *scheme = NULL;
    DWORD e = PowerGetActiveScheme(NULL, &scheme);
    if (e != ERROR_SUCCESS) return e;
    e = onBattery ? PowerWriteDCValueIndex(NULL, scheme, &GUID_SLEEP_SUB, &GUID_RTCWAKE, 1)
                  : PowerWriteACValueIndex(NULL, scheme, &GUID_SLEEP_SUB, &GUID_RTCWAKE, 1);
    if (e == ERROR_SUCCESS) e = PowerSetActiveScheme(NULL, scheme);   // 書いた値を今の設定に反映する
    LocalFree(scheme);
    return e;
}

// ---- タスクトレイ ----

static void tray_update_tip(void) {
    ULONGLONG due = 0; const Alarm *na = NULL;
    for (int i = 0; i < g_count; i++)
        if (g_alarms[i].enabled && g_alarms[i].next && (!due || g_alarms[i].next < due)) { due = g_alarms[i].next; na = &g_alarms[i]; }
    if (na) {
        SYSTEMTIME s; utc_to_local(due, &s);
        swprintf(g_nid.szTip, ARRAYSIZE(g_nid.szTip), L"wake-mart\n次: %d/%d(%ls) %02d:%02d %ls",
                 s.wMonth, s.wDay, WDAY[s.wDayOfWeek], s.wHour, s.wMinute, na->name);
    } else {
        wcscpy(g_nid.szTip, L"wake-mart\n予定なし");
    }
    g_nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void tray_add(void) {
    ZeroMemory(&g_nid, sizeof g_nid);
    g_nid.cbSize = sizeof g_nid;
    g_nid.hWnd = g_main;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = g_iconSmall;
    wcscpy(g_nid.szTip, APP_NAME);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
    tray_update_tip();
}

static void tray_balloon(const wchar_t *title, const wchar_t *text) {
    g_nid.uFlags = NIF_INFO;
    g_nid.dwInfoFlags = NIIF_INFO;
    wcsncpy(g_nid.szInfoTitle, title, ARRAYSIZE(g_nid.szInfoTitle) - 1);
    wcsncpy(g_nid.szInfo, text, ARRAYSIZE(g_nid.szInfo) - 1);
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

// ---- 音（MCI。wav/mp3/wma などを鳴らせる） ----

static void resolve_sound(const wchar_t *file, wchar_t *out, size_t n) {
    if (file[0]) { ExpandEnvironmentStringsW(file, out, (DWORD)n); return; }
    wchar_t win[MAX_PATH];
    GetWindowsDirectoryW(win, MAX_PATH);
    swprintf(out, n, L"%ls\\Media\\Alarm01.wav", win);
}

// 鳴らし始める。notify に MM_MCINOTIFY が届く。失敗したら FALSE
static BOOL mci_play(const wchar_t *alias, const wchar_t *file, HWND notify) {
    wchar_t cmd[MAX_PATH + 96];
    swprintf(cmd, ARRAYSIZE(cmd), L"open \"%ls\" type mpegvideo alias %ls", file, alias);
    MCIERROR e = mciSendStringW(cmd, NULL, 0, NULL);
    if (e) { logw(L"音を開けません（%ls, mci=%lu）", file, e); return FALSE; }
    swprintf(cmd, ARRAYSIZE(cmd), L"play %ls notify", alias);
    if (mciSendStringW(cmd, NULL, 0, notify)) {
        swprintf(cmd, ARRAYSIZE(cmd), L"close %ls", alias);
        mciSendStringW(cmd, NULL, 0, NULL);
        return FALSE;
    }
    return TRUE;
}

static void mci_replay(const wchar_t *alias, HWND notify) {
    wchar_t cmd[64];
    swprintf(cmd, ARRAYSIZE(cmd), L"play %ls from 0 notify", alias);
    mciSendStringW(cmd, NULL, 0, notify);
}

static void mci_close(const wchar_t *alias) {
    wchar_t cmd[64];
    swprintf(cmd, ARRAYSIZE(cmd), L"close %ls", alias);
    mciSendStringW(cmd, NULL, 0, NULL);
}

// ---- 鳴ったときの窓 ----

typedef struct {
    Alarm a;
    ULONGLONG sched;
    BOOL late, snoozed;
    int sound;              // 0=鳴らしていない 1=MCI 2=PlaySound
    ULONGLONG soundStart;
    wchar_t alias[32];
    HFONT titleFont;
} Popup;

static int popups_wanting_display(void) {
    int n = 0;
    for (int i = 0; i < g_popupCount; i++) {
        Popup *p = (Popup *)GetWindowLongPtrW(g_popups[i], DWLP_USER);
        if (p && p->a.display) n++;
    }
    return n;
}

static void popup_stop_sound(Popup *p) {
    if (p->sound == 1) mci_close(p->alias);
    else if (p->sound == 2) PlaySoundW(NULL, NULL, 0);
    p->sound = 0;
}

static void add_snooze(int id, int minutes) {
    if (g_snoozeCount >= MAX_SNOOZES || minutes <= 0) return;
    g_snoozes[g_snoozeCount].id = id;
    g_snoozes[g_snoozeCount].due = now_utc() + (ULONGLONG)minutes * 60ULL * TPS;
    g_snoozeCount++;
    logw(L"スヌーズ: id=%d %d 分後", id, minutes);
    arm_wake(FALSE);
}

static INT_PTR CALLBACK popup_proc(HWND d, UINT m, WPARAM wp, LPARAM lp) {
    Popup *p = (Popup *)GetWindowLongPtrW(d, DWLP_USER);
    switch (m) {
    case WM_INITDIALOG: {
        p = (Popup *)lp;
        SetWindowLongPtrW(d, DWLP_USER, (LONG_PTR)p);
        SendMessageW(d, WM_SETICON, ICON_BIG, (LPARAM)g_iconLarge);
        SendMessageW(d, WM_SETICON, ICON_SMALL, (LPARAM)g_iconSmall);

        LOGFONTW lf;
        GetObjectW((HFONT)SendMessageW(d, WM_GETFONT, 0, 0), sizeof lf, &lf);
        lf.lfWeight = FW_BOLD;
        lf.lfHeight = lf.lfHeight * 3 / 2;
        p->titleFont = CreateFontIndirectW(&lf);
        SendDlgItemMessageW(d, IDC_P_TITLE, WM_SETFONT, (WPARAM)p->titleFont, FALSE);
        SetDlgItemTextW(d, IDC_P_TITLE, p->a.name);

        SYSTEMTIME s; utc_to_local(p->sched, &s);
        wchar_t t[128];
        if (p->snoozed)   swprintf(t, 128, L"%02d:%02d:%02d（スヌーズ）", s.wHour, s.wMinute, s.wSecond);
        else if (p->late) swprintf(t, 128, L"%d/%d %02d:%02d:%02d の予定（遅れて実行）", s.wMonth, s.wDay, s.wHour, s.wMinute, s.wSecond);
        else              swprintf(t, 128, L"%02d:%02d:%02d", s.wHour, s.wMinute, s.wSecond);
        SetDlgItemTextW(d, IDC_P_TIME, t);
        SetDlgItemTextW(d, IDC_P_MSG, p->a.actMsg && p->a.msg[0] ? p->a.msg : L"");

        if (p->a.snooze > 0) {
            swprintf(t, 128, L"スヌーズ(&S) %d分", p->a.snooze);
            SetDlgItemTextW(d, IDC_P_SNOOZE, t);
        } else {
            ShowWindow(GetDlgItem(d, IDC_P_SNOOZE), SW_HIDE);
        }

        // 画面の中央に、複数あれば少しずつずらして出す
        RECT rc, wa;
        GetWindowRect(d, &rc);
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        int off = (g_popupCount % 8) * 24;
        SetWindowPos(d, HWND_TOPMOST,
                     wa.left + (wa.right - wa.left - (rc.right - rc.left)) / 2 + off,
                     wa.top + (wa.bottom - wa.top - (rc.bottom - rc.top)) / 2 + off,
                     0, 0, SWP_NOSIZE);

        if (p->a.actSound) {
            wchar_t file[MAX_PATH];
            resolve_sound(p->a.sound, file, MAX_PATH);
            swprintf(p->alias, ARRAYSIZE(p->alias), L"wm%llx", (unsigned long long)(UINT_PTR)d);
            if (mci_play(p->alias, file, d)) p->sound = 1;
            else if (PlaySoundW(file, NULL, SND_FILENAME | SND_ASYNC | SND_LOOP | SND_NODEFAULT)) p->sound = 2;
            p->soundStart = GetTickCount64();
        }
        SetTimer(d, 1, 1000, NULL);
        FLASHWINFO fw = { sizeof fw, d, FLASHW_ALL | FLASHW_TIMERNOFG, 0, 0 };
        FlashWindowEx(&fw);
        SetFocus(GetDlgItem(d, IDOK));    // メッセージ欄に入ると本文が全選択になる
        return FALSE;
    }
    case MM_MCINOTIFY:
        if (p && p->sound == 1 && wp == MCI_NOTIFY_SUCCESSFUL) mci_replay(p->alias, d);
        return TRUE;
    case WM_TIMER:
        if (p && p->sound && GetTickCount64() - p->soundStart > SOUND_MAX_SEC * 1000ULL) {
            popup_stop_sound(p);
            logw(L"音を %d 分で止めました: %ls", SOUND_MAX_SEC / 60, p->a.name);
        }
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_P_SNOOZE:
            add_snooze(p->a.id, p->a.snooze);
            DestroyWindow(d);
            return TRUE;
        case IDOK:
        case IDCANCEL:
            DestroyWindow(d);
            return TRUE;
        }
        break;
    case WM_DESTROY:
        if (p) {
            popup_stop_sound(p);
            if (p->titleFont) DeleteObject(p->titleFont);
            for (int i = 0; i < g_popupCount; i++)
                if (g_popups[i] == d) { g_popups[i] = g_popups[--g_popupCount]; break; }
            SetWindowLongPtrW(d, DWLP_USER, 0);
            free(p);
            update_exec_state();
        }
        return TRUE;
    }
    return FALSE;
}

static void popup_open(const Alarm *a, ULONGLONG sched, BOOL late, BOOL snoozed) {
    if (g_popupCount >= MAX_POPUPS) return;
    Popup *p = (Popup *)calloc(1, sizeof *p);
    if (!p) return;
    p->a = *a; p->sched = sched; p->late = late; p->snoozed = snoozed;
    g_popups[g_popupCount++] = NULL;   // WM_INITDIALOG 中の数え上げ用に先に枠を取る
    HWND d = CreateDialogParamW(g_inst, MAKEINTRESOURCEW(IDD_POPUP), NULL, popup_proc, (LPARAM)p);
    if (!d) { g_popupCount--; free(p); return; }
    g_popups[g_popupCount - 1] = d;
    ShowWindow(d, SW_SHOWNOACTIVATE);  // キー入力を横取りしない
}

// ---- 電源操作 ----

static void enable_shutdown_privilege(void) {
    HANDLE t;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &t)) return;
    TOKEN_PRIVILEGES tp;
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    LookupPrivilegeValueW(NULL, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid);
    AdjustTokenPrivileges(t, FALSE, &tp, 0, NULL, NULL);
    CloseHandle(t);
}

static void do_power(int op) {
    DWORD reason = SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_MINOR_OTHER | SHTDN_REASON_FLAG_PLANNED;
    logw(L"電源操作: %ls", POWER_NAMES[op]);
    switch (op) {
    case PW_SLEEP:     arm_wake(TRUE); SetSuspendState(FALSE, FALSE, FALSE); break;
    case PW_HIBERNATE: arm_wake(TRUE); SetSuspendState(TRUE, FALSE, FALSE); break;
    case PW_SHUTDOWN:  save_alarms(); enable_shutdown_privilege(); ExitWindowsEx(EWX_POWEROFF, reason); break;
    case PW_REBOOT:    save_alarms(); enable_shutdown_privilege(); ExitWindowsEx(EWX_REBOOT, reason); break;
    case PW_LOGOFF:    save_alarms(); ExitWindowsEx(EWX_LOGOFF, 0); break;
    case PW_MONITOR:   SendMessageW(g_main, WM_SYSCOMMAND, SC_MONITORPOWER, 2); break;
    }
}

static void countdown_text(HWND d) {
    wchar_t s[256];
    swprintf(s, 256, L"「%ls」のあと、%d 秒後に%lsします。", g_cdName, g_cdLeft, POWER_NAMES[g_cdOp]);
    SetDlgItemTextW(d, IDC_CD_TEXT, s);
}

static INT_PTR CALLBACK countdown_proc(HWND d, UINT m, WPARAM wp, LPARAM lp) {
    (void)lp;
    switch (m) {
    case WM_INITDIALOG: {
        SendMessageW(d, WM_SETICON, ICON_BIG, (LPARAM)g_iconLarge);
        SendMessageW(d, WM_SETICON, ICON_SMALL, (LPARAM)g_iconSmall);
        countdown_text(d);
        RECT rc, wa;
        GetWindowRect(d, &rc);
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        SetWindowPos(d, HWND_TOPMOST, wa.left + (wa.right - wa.left - (rc.right - rc.left)) / 2,
                     wa.top + (wa.bottom - wa.top - (rc.bottom - rc.top)) / 2, 0, 0, SWP_NOSIZE);
        SetTimer(d, 1, 1000, NULL);
        SetFocus(GetDlgItem(d, IDCANCEL));   // うっかり Enter で実行しないよう、取り消し側に置く
        return FALSE;
    }
    case WM_TIMER:
        if (--g_cdLeft > 0) { countdown_text(d); return TRUE; }
        // fallthrough
    case WM_APP: {
        int op = g_cdOp;
        DestroyWindow(d);
        do_power(op);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_CD_NOW) { PostMessageW(d, WM_APP, 0, 0); return TRUE; }
        if (LOWORD(wp) == IDCANCEL) {
            logw(L"電源操作を取り消しました: %ls", POWER_NAMES[g_cdOp]);
            DestroyWindow(d);
            return TRUE;
        }
        break;
    case WM_DESTROY:
        g_countdown = NULL;
        update_exec_state();
        return TRUE;
    }
    return FALSE;
}

static void power_start(int op, const wchar_t *name) {
    if (op <= PW_NONE || op >= PW_COUNT) return;
    if (op == PW_MONITOR) { do_power(op); return; }
    if (g_countdown) { logw(L"電源操作の確認中のため %ls を見送りました", POWER_NAMES[op]); return; }
    g_cdOp = op; g_cdLeft = COUNTDOWN_SEC;
    wcsncpy(g_cdName, name, ARRAYSIZE(g_cdName) - 1);
    g_countdown = CreateDialogParamW(g_inst, MAKEINTRESOURCEW(IDD_COUNTDOWN), NULL, countdown_proc, 0);
    if (g_countdown) ShowWindow(g_countdown, SW_SHOWNOACTIVATE);
    update_exec_state();
}

// ---- プログラム実行 ----

// 実行できたら TRUE。wait なら *proc にプロセスを返す（無ければ NULL）
static BOOL run_command(const Alarm *a, HANDLE *proc) {
    wchar_t file[MAX_PATH], args[2048], dir[MAX_PATH];
    ExpandEnvironmentStringsW(a->cmd, file, MAX_PATH);
    ExpandEnvironmentStringsW(a->args, args, ARRAYSIZE(args));
    ExpandEnvironmentStringsW(a->dir, dir, MAX_PATH);
    if (!dir[0] && wcschr(file, L'\\')) {      // 未指定ならプログラムのあるフォルダ
        wcscpy(dir, file);
        wchar_t *sl = wcsrchr(dir, L'\\');
        if (sl) *sl = 0;
    }
    static const int SHOW[3] = { SW_SHOWNORMAL, SW_SHOWMINNOACTIVE, SW_HIDE };
    SHELLEXECUTEINFOW sei;
    ZeroMemory(&sei, sizeof sei);
    sei.cbSize = sizeof sei;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
    sei.lpFile = file;
    sei.lpParameters = args[0] ? args : NULL;
    sei.lpDirectory = dir[0] ? dir : NULL;
    sei.nShow = SHOW[a->show];
    if (!ShellExecuteExW(&sei)) {
        DWORD e = GetLastError();
        logw(L"実行できません: %ls（err=%lu）", file, e);
        wchar_t s[300]; swprintf(s, 300, L"「%ls」のプログラムを実行できませんでした（エラー %lu）。\n%ls", a->name, e, file);
        tray_balloon(L"wake-mart", s);
        *proc = NULL;
        return FALSE;
    }
    logw(L"実行: %ls %ls", file, args);
    *proc = sei.hProcess;
    return TRUE;
}

// ---- 鳴らす ----

static void fire(const Alarm *a, ULONGLONG sched, BOOL late, BOOL snoozed, BOOL test) {
    double lateSec = (double)(LONGLONG)(now_utc() - sched) / TPS;
    logw(L"アラーム: id=%d「%ls」 遅れ %.1f 秒%ls%ls", a->id, a->name, lateSec,
         snoozed ? L"（スヌーズ）" : L"", test ? L"（テスト）" : L"");
    SetThreadExecutionState(ES_SYSTEM_REQUIRED);     // 起こされた直後の無人スリープを遠ざける
    g_lastFireTick = GetTickCount64();
    if (a->display) wake_display();

    BOOL popup = a->actMsg || a->actSound;
    if (popup) popup_open(a, sched, late, snoozed);
    if (snoozed) { update_exec_state(); return; }

    BOOL powerLater = FALSE;
    if (a->actCmd && a->cmd[0]) {
        HANDLE h = NULL;
        if (run_command(a, &h) && h) {
            if (a->cmdWait && g_jobCount < MAX_JOBS) {
                Job *j = &g_jobs[g_jobCount++];
                j->proc = h;
                j->power = test ? PW_NONE : a->power;
                wcsncpy(j->name, a->name, ARRAYSIZE(j->name) - 1);
                powerLater = TRUE;
            } else {
                CloseHandle(h);
            }
        }
    }
    if (!test && a->power && !powerLater) power_start(a->power, a->name);
    if (!popup) {
        wchar_t s[200];
        swprintf(s, 200, L"「%ls」を実行しました。", a->name);
        tray_balloon(L"wake-mart", s);
    }
    update_exec_state();
}

static void reap_jobs(void) {
    for (int i = 0; i < g_jobCount; ) {
        if (WaitForSingleObject(g_jobs[i].proc, 0) == WAIT_OBJECT_0) {
            DWORD code = 0;
            GetExitCodeProcess(g_jobs[i].proc, &code);
            logw(L"プログラム終了: 「%ls」 終了コード %lu", g_jobs[i].name, code);
            CloseHandle(g_jobs[i].proc);
            Job j = g_jobs[i];
            g_jobs[i] = g_jobs[--g_jobCount];
            if (j.power) power_start(j.power, j.name);
        } else i++;
    }
}

// ---- メイン画面 ----

static void list_refresh(int selectId);

static void tick(void) {
    ULONGLONG now = now_utc();
    BOOL changed = FALSE, fired = FALSE;
    for (int i = 0; i < g_count; i++) {
        Alarm *a = &g_alarms[i];
        if (!a->enabled || !a->next || now < a->next) continue;
        ULONGLONG sched = a->next;
        BOOL late = now - sched > LATE_GRACE_SEC * TPS;
        if (a->kind == K_ONCE) { a->enabled = FALSE; a->next = 0; changed = TRUE; }
        else a->next = next_after(a, now);
        fired = TRUE;
        if (!late || a->catchup) {
            Alarm copy = *a;
            fire(&copy, sched, late, FALSE, FALSE);
            a = find_alarm(copy.id);            // fire 中に配列が変わっても大丈夫なように
            if (!a) break;
        } else {
            wchar_t s[64]; fmt_local(sched, s, 64);
            logw(L"取りこぼし: id=%d「%ls」 予定 %ls（実行しない設定）", a->id, a->name, s);
        }
    }
    for (int i = 0; i < g_snoozeCount; ) {
        if (now >= g_snoozes[i].due) {
            Snooze sn = g_snoozes[i];
            g_snoozes[i] = g_snoozes[--g_snoozeCount];
            const Alarm *a = find_alarm(sn.id);
            if (a) { Alarm copy = *a; fire(&copy, sn.due, FALSE, TRUE, FALSE); }
        } else i++;
    }
    reap_jobs();
    arm_wake(FALSE);
    update_exec_state();
    if (changed) save_alarms();
    if (fired) list_refresh(-1);
}

static void update_status(void) {
    if (!g_main) return;
    wchar_t s[512], when[64];
    if (g_wakeDue) {
        const Alarm *a = find_alarm(g_wakeId);
        fmt_local(g_wakeDue, when, 64);
        swprintf(s, 512, L"次にスリープ・休止状態から起こすアラーム: %ls 「%ls」", when, a ? a->name : L"");
    } else {
        wcscpy(s, L"スリープ・休止状態から起こす予定はありません");
    }
    BOOL batt;
    int v = read_rtcwake(&batt);
    const wchar_t *src = batt ? L"バッテリー" : L"AC 電源";
    wchar_t w[256] = L"";
    if (v == 0)      swprintf(w, 256, L"\n⚠ 今の電源（%ls）ではスリープ解除タイマーが「無効」のため、スリープ・休止状態から起こせません。", src);
    else if (v == 2) swprintf(w, 256, L"\n⚠ 今の電源（%ls）ではスリープ解除タイマーが「重要なもののみ」のため、起こせないことがあります。", src);
    wcsncat(s, w, 511 - wcslen(s));
    SetDlgItemTextW(g_main, IDC_STATUS, s);
    ShowWindow(GetDlgItem(g_main, IDC_FIX_RTC), v == 0 || v == 2 ? SW_SHOW : SW_HIDE);
}

static int selected_id(void) {
    int i = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
    if (i < 0) return 0;
    LVITEMW it = { 0 };
    it.mask = LVIF_PARAM; it.iItem = i;
    ListView_GetItem(g_list, &it);
    return (int)it.lParam;
}

static void list_refresh(int selectId) {
    if (selectId < 0) selectId = selected_id();
    g_listUpdating = TRUE;
    SendMessageW(g_list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_list);
    for (int i = 0; i < g_count; i++) {
        const Alarm *a = &g_alarms[i];
        wchar_t buf[512];
        LVITEMW it = { 0 };
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = i;
        it.pszText = (wchar_t *)a->name;
        it.lParam = a->id;
        int row = ListView_InsertItem(g_list, &it);
        describe_schedule(a, buf, 512);
        ListView_SetItemText(g_list, row, 1, buf);
        if (!a->enabled) wcscpy(buf, L"（無効）");
        else if (a->next) fmt_local(a->next, buf, 512);
        else wcscpy(buf, L"—");
        ListView_SetItemText(g_list, row, 2, buf);
        describe_actions(a, buf, 512);
        ListView_SetItemText(g_list, row, 3, buf);
        ListView_SetItemText(g_list, row, 4, a->wake ? L"○" : L"");
        ListView_SetCheckState(g_list, row, a->enabled);
        if (a->id == selectId) ListView_SetItemState(g_list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    SendMessageW(g_list, WM_SETREDRAW, TRUE, 0);
    g_listUpdating = FALSE;
    BOOL sel = selected_id() != 0;
    EnableWindow(GetDlgItem(g_main, IDC_EDIT), sel);
    EnableWindow(GetDlgItem(g_main, IDC_COPY), sel);
    EnableWindow(GetDlgItem(g_main, IDC_DEL), sel);
    tray_update_tip();
}

// ---- アラームの設定画面 ----

typedef struct { Alarm a; BOOL testing; } EditCtx;

static void set_check(HWND d, int id, BOOL on) { CheckDlgButton(d, id, on ? BST_CHECKED : BST_UNCHECKED); }
static BOOL get_check(HWND d, int id) { return IsDlgButtonChecked(d, id) == BST_CHECKED; }
static void show_ctl(HWND d, int id, BOOL on) { ShowWindow(GetDlgItem(d, id), on ? SW_SHOW : SW_HIDE); }
static void enable_ctl(HWND d, int id, BOOL on) { EnableWindow(GetDlgItem(d, id), on); }

static void edit_layout(HWND d) {
    int k = (int)SendDlgItemMessageW(d, IDC_KIND, CB_GETCURSEL, 0, 0);
    BOOL date = k == K_ONCE || k == K_INTERVAL;
    show_ctl(d, IDC_DATE_LABEL, date);
    show_ctl(d, IDC_DATE, date);
    SetDlgItemTextW(d, IDC_DATE_LABEL, k == K_INTERVAL ? L"開始日:" : L"日付:");
    for (int i = 0; i < 7; i++) show_ctl(d, IDC_WD0 + i, k == K_WEEKLY);
    show_ctl(d, IDC_MDAY, k == K_MONTHLY);
    show_ctl(d, IDC_MDAY_L2, k == K_MONTHLY);
    show_ctl(d, IDC_INTERVAL, k == K_INTERVAL);
    show_ctl(d, IDC_INTERVAL_L, k == K_INTERVAL);
    SetDlgItemTextW(d, IDC_ROW3_LABEL, k == K_WEEKLY ? L"曜日:" : k == K_MONTHLY ? L"日にち:" : k == K_INTERVAL ? L"間隔:" : L"");

    BOOL msg = get_check(d, IDC_ACT_MSG), snd = get_check(d, IDC_ACT_SOUND), cmd = get_check(d, IDC_ACT_CMD);
    enable_ctl(d, IDC_MSG, msg);
    enable_ctl(d, IDC_SOUND, snd);
    enable_ctl(d, IDC_SOUND_BROWSE, snd);
    enable_ctl(d, IDC_CMD, cmd);
    enable_ctl(d, IDC_CMD_BROWSE, cmd);
    enable_ctl(d, IDC_ARGS, cmd);
    enable_ctl(d, IDC_DIR, cmd);
    enable_ctl(d, IDC_SHOW, cmd);
    enable_ctl(d, IDC_CMD_WAIT, cmd);
    int pw = (int)SendDlgItemMessageW(d, IDC_POWER, CB_GETCURSEL, 0, 0);
    show_ctl(d, IDC_POWER_NOTE, pw > PW_NONE && pw != PW_MONITOR);
}

static void edit_read(HWND d, Alarm *a) {
    GetDlgItemTextW(d, IDC_NAME, a->name, ARRAYSIZE(a->name));
    a->enabled = get_check(d, IDC_ENABLED);
    a->kind = (int)SendDlgItemMessageW(d, IDC_KIND, CB_GETCURSEL, 0, 0);
    if (a->kind < 0) a->kind = K_ONCE;
    SYSTEMTIME dt, tm;
    SendDlgItemMessageW(d, IDC_DATE, DTM_GETSYSTEMTIME, 0, (LPARAM)&dt);
    SendDlgItemMessageW(d, IDC_TIME, DTM_GETSYSTEMTIME, 0, (LPARAM)&tm);
    ZeroMemory(&a->start, sizeof a->start);
    a->start.wYear = dt.wYear; a->start.wMonth = dt.wMonth; a->start.wDay = dt.wDay;
    a->start.wHour = tm.wHour; a->start.wMinute = tm.wMinute; a->start.wSecond = tm.wSecond;
    u64_to_st(st_to_u64(&a->start), &a->start);
    a->weekdays = 0;
    for (int i = 0; i < 7; i++) if (get_check(d, IDC_WD0 + i)) a->weekdays |= 1 << i;
    a->mday = (int)GetDlgItemInt(d, IDC_MDAY, NULL, FALSE);
    a->interval = (int)GetDlgItemInt(d, IDC_INTERVAL, NULL, FALSE);
    a->actMsg = get_check(d, IDC_ACT_MSG);
    GetDlgItemTextW(d, IDC_MSG, a->msg, ARRAYSIZE(a->msg));
    a->actSound = get_check(d, IDC_ACT_SOUND);
    GetDlgItemTextW(d, IDC_SOUND, a->sound, ARRAYSIZE(a->sound));
    a->actCmd = get_check(d, IDC_ACT_CMD);
    GetDlgItemTextW(d, IDC_CMD, a->cmd, ARRAYSIZE(a->cmd));
    GetDlgItemTextW(d, IDC_ARGS, a->args, ARRAYSIZE(a->args));
    GetDlgItemTextW(d, IDC_DIR, a->dir, ARRAYSIZE(a->dir));
    a->show = (int)SendDlgItemMessageW(d, IDC_SHOW, CB_GETCURSEL, 0, 0);
    if (a->show < 0) a->show = 0;
    a->cmdWait = get_check(d, IDC_CMD_WAIT);
    a->power = (int)SendDlgItemMessageW(d, IDC_POWER, CB_GETCURSEL, 0, 0);
    if (a->power < 0) a->power = PW_NONE;
    a->wake = get_check(d, IDC_WAKE);
    a->display = get_check(d, IDC_DISPLAY);
    a->catchup = get_check(d, IDC_CATCHUP);
    a->snooze = (int)GetDlgItemInt(d, IDC_SNOOZE, NULL, FALSE);
}

static void edit_preview(HWND d) {
    EditCtx *c = (EditCtx *)GetWindowLongPtrW(d, DWLP_USER);
    if (!c) return;
    Alarm t = c->a;
    edit_read(d, &t);
    t.enabled = TRUE;
    ULONGLONG n = next_after(&t, now_utc());
    wchar_t s[96], w[64];
    if (n) { fmt_local(n, w, 64); swprintf(s, 96, L"次回: %ls", w); }
    else if (t.kind == K_ONCE) wcscpy(s, L"次回: なし（日時が過ぎています）");
    else wcscpy(s, L"次回: なし");
    SetDlgItemTextW(d, IDC_PREVIEW, s);
}

static void fill_combo(HWND d, int id, const wchar_t **names, int n, int sel) {
    for (int i = 0; i < n; i++) SendDlgItemMessageW(d, id, CB_ADDSTRING, 0, (LPARAM)names[i]);
    SendDlgItemMessageW(d, id, CB_SETCURSEL, sel, 0);
}

static BOOL browse_file(HWND d, int editId, const wchar_t *filter, const wchar_t *title) {
    wchar_t file[MAX_PATH];
    GetDlgItemTextW(d, editId, file, MAX_PATH);
    OPENFILENAMEW of;
    ZeroMemory(&of, sizeof of);
    of.lStructSize = sizeof of;
    of.hwndOwner = d;
    of.lpstrFilter = filter;
    of.lpstrFile = file;
    of.nMaxFile = MAX_PATH;
    of.lpstrTitle = title;
    of.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&of)) return FALSE;
    SetDlgItemTextW(d, editId, file);
    return TRUE;
}

static void edit_stop_test(HWND d, EditCtx *c) {
    if (!c->testing) return;
    mci_close(L"wmtest");
    PlaySoundW(NULL, NULL, 0);
    c->testing = FALSE;
    SetDlgItemTextW(d, IDC_SOUND_TEST, L"試聴");
}

static INT_PTR CALLBACK edit_proc(HWND d, UINT m, WPARAM wp, LPARAM lp) {
    EditCtx *c = (EditCtx *)GetWindowLongPtrW(d, DWLP_USER);
    switch (m) {
    case WM_INITDIALOG: {
        c = (EditCtx *)lp;
        const Alarm *a = &c->a;
        SendMessageW(d, WM_SETICON, ICON_SMALL, (LPARAM)g_iconSmall);
        SetDlgItemTextW(d, IDC_NAME, a->name);
        SendDlgItemMessageW(d, IDC_NAME, EM_LIMITTEXT, ARRAYSIZE(a->name) - 1, 0);
        SendDlgItemMessageW(d, IDC_MSG, EM_LIMITTEXT, ARRAYSIZE(a->msg) - 64, 0);
        set_check(d, IDC_ENABLED, a->enabled);
        fill_combo(d, IDC_KIND, KIND_NAMES, K_COUNT, a->kind);
        SendDlgItemMessageW(d, IDC_DATE, DTM_SETFORMATW, 0, (LPARAM)L"yyyy'/'MM'/'dd'('ddd')'");
        SendDlgItemMessageW(d, IDC_TIME, DTM_SETFORMATW, 0, (LPARAM)L"HH':'mm':'ss");
        SendDlgItemMessageW(d, IDC_DATE, DTM_SETSYSTEMTIME, GDT_VALID, (LPARAM)&a->start);
        SendDlgItemMessageW(d, IDC_TIME, DTM_SETSYSTEMTIME, GDT_VALID, (LPARAM)&a->start);
        for (int i = 0; i < 7; i++) set_check(d, IDC_WD0 + i, a->weekdays >> i & 1);
        SetDlgItemInt(d, IDC_MDAY, (UINT)a->mday, FALSE);
        SetDlgItemInt(d, IDC_INTERVAL, (UINT)a->interval, FALSE);
        SetDlgItemInt(d, IDC_AFTER, 5, FALSE);
        set_check(d, IDC_ACT_MSG, a->actMsg);
        SetDlgItemTextW(d, IDC_MSG, a->msg);
        set_check(d, IDC_ACT_SOUND, a->actSound);
        SetDlgItemTextW(d, IDC_SOUND, a->sound);
        SendDlgItemMessageW(d, IDC_SOUND, EM_SETCUEBANNER, TRUE, (LPARAM)L"空欄なら Windows の Alarm01.wav");
        set_check(d, IDC_ACT_CMD, a->actCmd);
        SetDlgItemTextW(d, IDC_CMD, a->cmd);
        SetDlgItemTextW(d, IDC_ARGS, a->args);
        SetDlgItemTextW(d, IDC_DIR, a->dir);
        SendDlgItemMessageW(d, IDC_DIR, EM_SETCUEBANNER, TRUE, (LPARAM)L"空欄ならファイルのある場所");
        fill_combo(d, IDC_SHOW, SHOW_NAMES, 3, a->show);
        set_check(d, IDC_CMD_WAIT, a->cmdWait);
        fill_combo(d, IDC_POWER, POWER_NAMES, PW_COUNT, a->power);
        set_check(d, IDC_WAKE, a->wake);
        set_check(d, IDC_DISPLAY, a->display);
        set_check(d, IDC_CATCHUP, a->catchup);
        SetDlgItemInt(d, IDC_SNOOZE, (UINT)a->snooze, FALSE);
        SetWindowLongPtrW(d, DWLP_USER, (LONG_PTR)c);
        edit_layout(d);
        edit_preview(d);
        return TRUE;
    }
    case WM_NOTIFY:
        if (((NMHDR *)lp)->code == DTN_DATETIMECHANGE) edit_preview(d);
        break;
    case MM_MCINOTIFY:
        if (c) edit_stop_test(d, c);
        return TRUE;
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (!c) break;
        if ((code == EN_CHANGE && id != IDC_AFTER) || code == CBN_SELCHANGE ||
            (code == BN_CLICKED && id != IDOK && id != IDCANCEL)) {
            edit_layout(d);
            edit_preview(d);
        }
        switch (id) {
        case IDC_AFTER_SET: {
            SYSTEMTIME now, t;
            GetLocalTime(&now);
            now.wMilliseconds = 0;
            u64_to_st(st_to_u64(&now) + (ULONGLONG)GetDlgItemInt(d, IDC_AFTER, NULL, FALSE) * 60ULL * TPS, &t);
            SendDlgItemMessageW(d, IDC_KIND, CB_SETCURSEL, K_ONCE, 0);
            SendDlgItemMessageW(d, IDC_DATE, DTM_SETSYSTEMTIME, GDT_VALID, (LPARAM)&t);
            SendDlgItemMessageW(d, IDC_TIME, DTM_SETSYSTEMTIME, GDT_VALID, (LPARAM)&t);
            set_check(d, IDC_ENABLED, TRUE);
            edit_layout(d);
            edit_preview(d);
            return TRUE;
        }
        case IDC_SOUND_BROWSE:
            browse_file(d, IDC_SOUND, L"音声ファイル (*.wav;*.mp3;*.wma;*.m4a)\0*.wav;*.mp3;*.wma;*.m4a\0すべてのファイル (*.*)\0*.*\0", L"音声ファイルを選ぶ");
            return TRUE;
        case IDC_CMD_BROWSE:
            browse_file(d, IDC_CMD, L"プログラム (*.exe;*.bat;*.cmd;*.lnk;*.vbs)\0*.exe;*.bat;*.cmd;*.lnk;*.vbs\0すべてのファイル (*.*)\0*.*\0", L"実行するファイルを選ぶ");
            return TRUE;
        case IDC_SOUND_TEST: {
            if (c->testing) { edit_stop_test(d, c); return TRUE; }
            wchar_t raw[MAX_PATH], file[MAX_PATH];
            GetDlgItemTextW(d, IDC_SOUND, raw, MAX_PATH);
            resolve_sound(raw, file, MAX_PATH);
            if (mci_play(L"wmtest", file, d) ||
                PlaySoundW(file, NULL, SND_FILENAME | SND_ASYNC | SND_NODEFAULT)) {
                c->testing = TRUE;
                SetDlgItemTextW(d, IDC_SOUND_TEST, L"停止");
            } else {
                MessageBoxW(d, L"この音声ファイルを鳴らせません。", APP_NAME, MB_ICONWARNING);
            }
            return TRUE;
        }
        case IDC_TEST: {
            Alarm t = c->a;
            edit_read(d, &t);
            fire(&t, now_utc(), FALSE, FALSE, TRUE);
            return TRUE;
        }
        case IDOK: {
            Alarm t = c->a;
            edit_read(d, &t);
            const wchar_t *err = NULL;
            if (!t.name[0]) wcscpy(t.name, L"アラーム");
            if (t.kind == K_WEEKLY && !t.weekdays) err = L"曜日を 1 つ以上選んでください。";
            else if (t.kind == K_MONTHLY && (t.mday < 1 || t.mday > 31)) err = L"日にちは 1〜31 で指定してください。";
            else if (t.kind == K_INTERVAL && t.interval < 1) err = L"間隔は 1 分以上にしてください。";
            else if (!t.actMsg && !t.actSound && !t.actCmd && !t.power) err = L"動作を 1 つ以上選んでください。";
            else if (t.actCmd && !t.cmd[0]) err = L"実行するファイルを指定してください。";
            if (err) { MessageBoxW(d, err, APP_NAME, MB_ICONWARNING); return TRUE; }
            if (t.enabled && t.kind == K_ONCE && !next_after(&t, now_utc()) &&
                MessageBoxW(d, L"日時が過ぎているので、このままでは鳴りません。保存しますか？",
                            APP_NAME, MB_ICONQUESTION | MB_YESNO) != IDYES)
                return TRUE;
            edit_stop_test(d, c);
            c->a = t;
            EndDialog(d, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            edit_stop_test(d, c);
            EndDialog(d, IDCANCEL);
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

// 設定画面を開く。OK なら TRUE で *a が書き換わる
static BOOL edit_alarm(Alarm *a) {
    EditCtx *c = (EditCtx *)calloc(1, sizeof *c);
    if (!c) return FALSE;
    c->a = *a;
    BOOL ok = DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_EDIT), g_main, edit_proc, (LPARAM)c) == IDOK;
    if (ok) *a = c->a;
    free(c);
    return ok;
}

// ---- メイン画面の操作 ----

static void cmd_add(void) {
    if (g_count >= MAX_ALARMS) { MessageBoxW(g_main, L"これ以上アラームを増やせません。", APP_NAME, MB_ICONWARNING); return; }
    Alarm a;
    default_alarm(&a);
    if (!edit_alarm(&a)) return;
    a.id = g_nextId++;
    recompute_next(&a);
    g_alarms[g_count++] = a;
    save_alarms();
    logw(L"追加: id=%d「%ls」", a.id, a.name);
    list_refresh(a.id);
    arm_wake(FALSE);
}

static void cmd_edit(void) {
    Alarm *p = find_alarm(selected_id());
    if (!p) return;
    Alarm a = *p;
    if (!edit_alarm(&a)) return;
    p = find_alarm(a.id);                 // 開いている間に消えていないか
    if (!p) return;
    *p = a;
    recompute_next(p);
    save_alarms();
    logw(L"変更: id=%d「%ls」", a.id, a.name);
    list_refresh(a.id);
    arm_wake(FALSE);
}

static void cmd_copy(void) {
    Alarm *p = find_alarm(selected_id());
    if (!p || g_count >= MAX_ALARMS) return;
    Alarm a = *p;
    a.id = g_nextId++;
    wchar_t name[160];
    swprintf(name, 160, L"%ls のコピー", p->name);
    wcsncpy(a.name, name, ARRAYSIZE(a.name) - 1);
    recompute_next(&a);
    g_alarms[g_count++] = a;
    save_alarms();
    list_refresh(a.id);
    arm_wake(FALSE);
}

static void cmd_delete(void) {
    int id = selected_id();
    Alarm *p = find_alarm(id);
    if (!p) return;
    wchar_t s[256];
    swprintf(s, 256, L"「%ls」を削除しますか？", p->name);
    if (MessageBoxW(g_main, s, APP_NAME, MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2) != IDYES) return;
    p = find_alarm(id);
    if (!p) return;
    logw(L"削除: id=%d「%ls」", id, p->name);
    int i = (int)(p - g_alarms);
    memmove(&g_alarms[i], &g_alarms[i + 1], (size_t)(g_count - i - 1) * sizeof(Alarm));
    g_count--;
    save_alarms();
    list_refresh(0);
    arm_wake(FALSE);
}

static void show_main(void) {
    ShowWindow(g_main, IsIconic(g_main) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(g_main);
}

static void hide_main(void) {
    ShowWindow(g_main, SW_HIDE);
    if (!g_hideNoticeShown) {
        g_hideNoticeShown = TRUE;
        tray_balloon(L"wake-mart", L"タスクトレイで動いています。終了するときはアイコンを右クリックしてください。");
    }
}

static BOOL confirm_exit(void) {
    BOOL any = FALSE;
    for (int i = 0; i < g_count; i++) if (g_alarms[i].enabled && g_alarms[i].next) any = TRUE;
    if (!any) return TRUE;
    return MessageBoxW(g_main, L"終了すると、このあとのアラームは鳴らず、スリープからも起こしません。\n終了しますか？",
                       APP_NAME, MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2) == IDYES;
}

static void tray_menu(void) {
    HMENU mn = CreatePopupMenu();
    AppendMenuW(mn, MF_STRING, IDM_OPEN, L"開く(&O)");
    AppendMenuW(mn, MF_SEPARATOR, 0, NULL);
    wchar_t s[160];
    ULONGLONG due = 0; const Alarm *na = NULL;
    for (int i = 0; i < g_count; i++)
        if (g_alarms[i].enabled && g_alarms[i].next && (!due || g_alarms[i].next < due)) { due = g_alarms[i].next; na = &g_alarms[i]; }
    if (na) { wchar_t w[64]; fmt_local(due, w, 64); swprintf(s, 160, L"次: %ls %ls", w, na->name); }
    else wcscpy(s, L"予定なし");
    AppendMenuW(mn, MF_STRING | MF_GRAYED, IDM_STATUS, s);
    AppendMenuW(mn, MF_SEPARATOR, 0, NULL);
    AppendMenuW(mn, MF_STRING, IDM_SLEEP_NOW, L"今すぐスリープ(&S)");
    AppendMenuW(mn, MF_STRING, IDM_HIBERNATE_NOW, L"今すぐ休止状態(&H)");
    AppendMenuW(mn, MF_SEPARATOR, 0, NULL);
    AppendMenuW(mn, MF_STRING, IDM_EXIT, L"終了(&X)");
    SetMenuDefaultItem(mn, IDM_OPEN, FALSE);
    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(g_main);          // これが無いとメニュー外クリックで閉じない
    TrackPopupMenu(mn, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_main, NULL);
    PostMessageW(g_main, WM_NULL, 0, 0);
    DestroyMenu(mn);
}

// 画面の大きさに合わせて部品を動かす
#define AN_RIGHT  1
#define AN_BOTTOM 2
#define AN_WIDTH  4
#define AN_HEIGHT 8
typedef struct { int id, flags; RECT rc; } Anchor;
static Anchor g_anchors[] = {
    { IDC_LIST, AN_WIDTH | AN_HEIGHT }, { IDC_ADD, AN_BOTTOM }, { IDC_EDIT, AN_BOTTOM },
    { IDC_COPY, AN_BOTTOM }, { IDC_DEL, AN_BOTTOM },
    { IDC_STATUS, AN_BOTTOM | AN_WIDTH }, { IDC_FIX_RTC, AN_BOTTOM | AN_RIGHT },
    { IDC_SLEEP_CLOSE, AN_BOTTOM | AN_RIGHT }, { IDCANCEL, AN_BOTTOM | AN_RIGHT },
};
static SIZE  g_baseClient;
static POINT g_minTrack;

static void anchors_init(HWND d) {
    RECT rc;
    GetClientRect(d, &rc);
    g_baseClient.cx = rc.right; g_baseClient.cy = rc.bottom;
    GetWindowRect(d, &rc);
    g_minTrack.x = rc.right - rc.left; g_minTrack.y = rc.bottom - rc.top;
    for (int i = 0; i < (int)ARRAYSIZE(g_anchors); i++) {
        GetWindowRect(GetDlgItem(d, g_anchors[i].id), &g_anchors[i].rc);
        MapWindowPoints(NULL, d, (POINT *)&g_anchors[i].rc, 2);
    }
}

static void anchors_apply(HWND d, int cx, int cy) {
    int dx = cx - g_baseClient.cx, dy = cy - g_baseClient.cy;
    HDWP h = BeginDeferWindowPos((int)ARRAYSIZE(g_anchors));
    for (int i = 0; i < (int)ARRAYSIZE(g_anchors) && h; i++) {
        const Anchor *a = &g_anchors[i];
        RECT r = a->rc;
        if (a->flags & AN_RIGHT)  { r.left += dx; r.right += dx; }
        if (a->flags & AN_BOTTOM) { r.top += dy; r.bottom += dy; }
        if (a->flags & AN_WIDTH)  r.right += dx;
        if (a->flags & AN_HEIGHT) r.bottom += dy;
        h = DeferWindowPos(h, GetDlgItem(d, a->id), NULL, r.left, r.top, r.right - r.left, r.bottom - r.top,
                           SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (h) EndDeferWindowPos(h);
    InvalidateRect(GetDlgItem(d, IDC_STATUS), NULL, TRUE);
}

static int dlu_x(HWND d, int n) { RECT r = { 0, 0, n, 0 }; MapDialogRect(d, &r); return r.right; }

static INT_PTR CALLBACK main_proc(HWND d, UINT m, WPARAM wp, LPARAM lp) {
    if (m == g_msgTaskbarCreated && m) { tray_add(); return TRUE; }
    switch (m) {
    case WM_INITDIALOG: {
        g_main = d;
        g_list = GetDlgItem(d, IDC_LIST);
        SetPropW(d, PROP_KEY, (HANDLE)(UINT_PTR)g_key);
        SendMessageW(d, WM_SETICON, ICON_BIG, (LPARAM)g_iconLarge);
        SendMessageW(d, WM_SETICON, ICON_SMALL, (LPARAM)g_iconSmall);
        if (g_customIni) {
            wchar_t t[MAX_PATH + 32];
            swprintf(t, ARRAYSIZE(t), L"wake-mart — %ls", g_iniPath);
            SetWindowTextW(d, t);
        }
        ListView_SetExtendedListViewStyle(g_list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        static const struct { const wchar_t *t; int w; } cols[] = {
            { L"名前", 80 }, { L"予定", 110 }, { L"次回", 90 }, { L"動作", 90 }, { L"復帰", 26 } };
        for (int i = 0; i < (int)ARRAYSIZE(cols); i++) {
            LVCOLUMNW c = { 0 };
            c.mask = LVCF_TEXT | LVCF_WIDTH;
            c.pszText = (wchar_t *)cols[i].t;
            c.cx = dlu_x(d, cols[i].w);
            ListView_InsertColumn(g_list, i, &c);
        }
        anchors_init(d);
        tray_add();
        list_refresh(0);
        arm_wake(TRUE);
        update_status();
        RegisterPowerSettingNotification(d, &GUID_ACDC, DEVICE_NOTIFY_WINDOW_HANDLE);
        RegisterPowerSettingNotification(d, &GUID_DISPLAY, DEVICE_NOTIFY_WINDOW_HANDLE);
        SetTimer(d, TIMER_TICK, TICK_MS, NULL);
        return TRUE;
    }
    case WM_TIMER:
        if (wp == TIMER_TICK) tick();
        return TRUE;
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) anchors_apply(d, LOWORD(lp), HIWORD(lp));
        return TRUE;
    case WM_GETMINMAXINFO:
        if (g_minTrack.x) { ((MINMAXINFO *)lp)->ptMinTrackSize = g_minTrack; return TRUE; }
        break;
    case WM_POWERBROADCAST:
        switch (wp) {
        case PBT_APMSUSPEND:
            logw(L"スリープ・休止状態に入ります");
            arm_wake(TRUE);               // 前倒しの時刻を過ぎていても起こせるよう付け直す
            break;
        case PBT_APMRESUMEAUTOMATIC:
            logw(L"復帰しました（自動）");
            tick();
            break;
        case PBT_APMRESUMESUSPEND:
            logw(L"復帰しました（利用者の操作）");
            break;
        case PBT_POWERSETTINGCHANGE: {
            const POWERBROADCAST_SETTING *ps = (const POWERBROADCAST_SETTING *)lp;
            if (IsEqualGUID(&ps->PowerSetting, &GUID_DISPLAY) && ps->DataLength >= sizeof(DWORD)) {
                static const wchar_t *st[] = { L"オフ", L"オン", L"減光" };
                DWORD v = *(const DWORD *)ps->Data;
                logw(L"画面: %ls", v < 3 ? st[v] : L"?");
            } else if (IsEqualGUID(&ps->PowerSetting, &GUID_ACDC)) {
                update_status();
            }
            break;
        }
        }
        SetWindowLongPtrW(d, DWLP_MSGRESULT, TRUE);
        return TRUE;
    case WM_TIMECHANGE:
        logw(L"時計が変更されました。予定を計算し直します");
        for (int i = 0; i < g_count; i++) recompute_next(&g_alarms[i]);
        list_refresh(-1);
        arm_wake(TRUE);
        return TRUE;
    case WM_TRAY:
        switch (LOWORD(lp)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
        case WM_LBUTTONDBLCLK:
            show_main();
            break;
        case WM_CONTEXTMENU:
            tray_menu();
            break;
        }
        return TRUE;
    case WM_APP_CMD:
        if (wp == CMD_SHOW) show_main();
        else if (wp == CMD_EXIT) { logw(L"-exit で終了します"); DestroyWindow(d); }
        return TRUE;
    case WM_NOTIFY: {
        NMHDR *nh = (NMHDR *)lp;
        if (nh->idFrom != IDC_LIST) break;
        if (nh->code == NM_DBLCLK) { cmd_edit(); return TRUE; }
        if (nh->code == LVN_KEYDOWN && ((NMLVKEYDOWN *)lp)->wVKey == VK_DELETE) { cmd_delete(); return TRUE; }
        if (nh->code == LVN_ITEMCHANGED && !g_listUpdating) {
            NMLISTVIEW *lv = (NMLISTVIEW *)lp;
            if ((lv->uChanged & LVIF_STATE) && ((lv->uNewState ^ lv->uOldState) & LVIS_STATEIMAGEMASK)) {
                Alarm *a = find_alarm((int)lv->lParam);
                BOOL on = ((lv->uNewState & LVIS_STATEIMAGEMASK) >> 12) == 2;
                if (a && a->enabled != on) {
                    a->enabled = on;
                    recompute_next(a);
                    save_alarms();
                    logw(L"%ls: id=%d「%ls」", on ? L"有効化" : L"無効化", a->id, a->name);
                    PostMessageW(d, WM_APP + 10, 0, 0);   // 通知の最中に一覧を作り直さない
                }
            }
            BOOL sel = selected_id() != 0;
            EnableWindow(GetDlgItem(d, IDC_EDIT), sel);
            EnableWindow(GetDlgItem(d, IDC_COPY), sel);
            EnableWindow(GetDlgItem(d, IDC_DEL), sel);
            return TRUE;
        }
        break;
    }
    case WM_APP + 10:
        list_refresh(-1);
        arm_wake(FALSE);
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_ADD:  cmd_add(); return TRUE;
        case IDOK:                         // 一覧で Enter
        case IDC_EDIT: cmd_edit(); return TRUE;
        case IDC_COPY: cmd_copy(); return TRUE;
        case IDC_DEL:  cmd_delete(); return TRUE;
        case IDM_SLEEP_NOW: do_power(PW_SLEEP); return TRUE;
        case IDM_HIBERNATE_NOW: do_power(PW_HIBERNATE); return TRUE;
        case IDC_FIX_RTC: {
            BOOL batt;
            read_rtcwake(&batt);
            wchar_t s[256];
            swprintf(s, 256, L"Windows の電源設定「スリープ解除タイマーの許可」（%ls）を「有効」に変更します。\nよろしいですか？",
                     batt ? L"バッテリー駆動" : L"電源に接続");
            if (MessageBoxW(d, s, APP_NAME, MB_ICONQUESTION | MB_YESNO) != IDYES) return TRUE;
            DWORD e = enable_rtcwake(batt);
            int v = read_rtcwake(&batt);
            logw(L"スリープ解除タイマーの許可を有効に変更: err=%lu 変更後=%d", e, v);
            if (e != ERROR_SUCCESS || v != 1) {
                swprintf(s, 256, L"変更できませんでした（エラー %lu、変更後の値 %d）。\n"
                                 L"コントロール パネルの電源オプションから変更してください。", e, v);
                MessageBoxW(d, s, APP_NAME, MB_ICONWARNING);
            }
            update_status();
            return TRUE;
        }
        case IDC_SLEEP_CLOSE:              // スタートメニューからスリープを選ぶ手間を省く
            ShowWindow(d, SW_HIDE);        // 復帰したときに画面が出たままにならないよう先に隠す
            do_power(PW_SLEEP);
            return TRUE;
        case IDCANCEL: hide_main(); return TRUE;
        case IDM_OPEN: show_main(); return TRUE;
        case IDM_EXIT:
            if (confirm_exit()) DestroyWindow(d);
            return TRUE;
        }
        break;
    case WM_CLOSE:
        hide_main();
        return TRUE;
    case WM_QUERYENDSESSION:
        SetWindowLongPtrW(d, DWLP_MSGRESULT, TRUE);
        return TRUE;
    case WM_ENDSESSION:
        if (wp) { save_alarms(); logw(L"Windows の終了に合わせて終了します"); }
        return TRUE;
    case WM_DESTROY:
        KillTimer(d, TIMER_TICK);
        while (g_popupCount > 0) DestroyWindow(g_popups[g_popupCount - 1]);
        if (g_countdown) DestroyWindow(g_countdown);
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        SetThreadExecutionState(ES_CONTINUOUS);
        CancelWaitableTimer(g_wakeTimer);
        RemovePropW(d, PROP_KEY);
        save_alarms();
        logw(L"終了");
        PostQuitMessage(0);
        return TRUE;
    }
    return FALSE;
}

// ---- 起動 ----

typedef struct { UINT key; HWND found; } FindCtx;

static BOOL CALLBACK find_proc(HWND h, LPARAM lp) {
    FindCtx *f = (FindCtx *)lp;
    wchar_t cls[64];
    if (GetClassNameW(h, cls, 64) && wcscmp(cls, MAIN_CLASS) == 0 &&
        (UINT)(UINT_PTR)GetPropW(h, PROP_KEY) == f->key) { f->found = h; return FALSE; }
    return TRUE;
}

static HWND find_instance(void) {
    FindCtx f = { g_key, NULL };
    EnumWindows(find_proc, (LPARAM)&f);
    return f.found;
}

static UINT path_key(const wchar_t *path) {
    UINT h = 2166136261u;
    for (; *path; path++) { h ^= (UINT)towlower(*path); h *= 16777619u; }
    return h ? h : 1;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show) {
    (void)prev; (void)cmdline;
    g_inst = inst;
    GetModuleFileNameW(NULL, g_exePath, MAX_PATH);

    BOOL tray = FALSE, doExit = FALSE;
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    wchar_t ini[MAX_PATH] = L"";
    for (int i = 1; argv && i < argc; i++) {
        if (!_wcsicmp(argv[i], L"-tray")) tray = TRUE;
        else if (!_wcsicmp(argv[i], L"-exit")) doExit = TRUE;
        else if (!_wcsicmp(argv[i], L"-config") && i + 1 < argc) wcsncpy(ini, argv[++i], MAX_PATH - 1);
    }
    if (argv) LocalFree(argv);
    if (ini[0]) {
        GetFullPathNameW(ini, MAX_PATH, g_iniPath, NULL);
        g_customIni = TRUE;
    } else {
        wcscpy(g_iniPath, g_exePath);
        wchar_t *sep = wcsrchr(g_iniPath, L'\\');
        wcscpy(sep ? sep + 1 : g_iniPath, L"wake-mart.ini");
    }
    wcscpy(g_logPath, g_iniPath);
    wchar_t *dot = wcsrchr(g_logPath, L'.');
    if (dot && dot > wcsrchr(g_logPath, L'\\')) wcscpy(dot, L".log");
    else wcscat(g_logPath, L".log");
    g_key = path_key(g_iniPath);

    // 同じ設定ファイルで動いているものがあれば、そちらに任せる
    wchar_t mutexName[64];
    swprintf(mutexName, 64, L"Local\\wake-mart-%08x", g_key);
    HANDLE mutex = CreateMutexW(NULL, TRUE, mutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = NULL;
        for (int i = 0; i < 20 && !(other = find_instance()); i++) Sleep(100);
        if (other) {
            DWORD pid; GetWindowThreadProcessId(other, &pid);
            AllowSetForegroundWindow(pid);
            PostMessageW(other, WM_APP_CMD, doExit ? CMD_EXIT : CMD_SHOW, 0);
        }
        return 0;
    }
    if (doExit) return 0;                 // 動いていなかった

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_LISTVIEW_CLASSES | ICC_DATE_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    g_iconLarge = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                    GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    g_iconSmall = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                    GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc = { sizeof wc };
    GetClassInfoExW(NULL, L"#32770", &wc);   // ダイアログの窓クラスを名前を変えて登録する
    wc.cbSize = sizeof wc;
    wc.hInstance = inst;
    wc.lpszClassName = MAIN_CLASS;
    wc.hIcon = g_iconLarge;
    wc.hIconSm = g_iconSmall;
    RegisterClassExW(&wc);

    g_wakeTimer = CreateWaitableTimerW(NULL, TRUE, NULL);
    logw(L"起動（%ls）", g_iniPath);
    load_alarms();
    for (int i = 0; i < g_count; i++) recompute_next(&g_alarms[i]);

    HWND d = CreateDialogParamW(inst, MAKEINTRESOURCEW(IDD_MAIN), NULL, main_proc, 0);
    if (!d) { logw(L"画面を作れません（err=%lu）", GetLastError()); return 1; }
    if (!tray) { ShowWindow(d, show == SW_SHOWMINNOACTIVE || show == SW_SHOWMINIMIZED ? show : SW_SHOWNORMAL); }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        HWND a = GetActiveWindow();
        if (a && IsDialogMessageW(a, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CloseHandle(g_wakeTimer);
    CoUninitialize();
    if (mutex) CloseHandle(mutex);
    return 0;
}
