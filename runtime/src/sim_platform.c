#define _CRT_SECURE_NO_WARNINGS
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE) && !defined(__APPLE__)
#define _POSIX_C_SOURCE 200809L
#endif
#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE
#endif
#include "sim_platform.h"
#include "sim_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

struct sim_mutex { SRWLOCK lock; };
struct sim_cond { CONDITION_VARIABLE cv; };

sim_mutex *sim_mutex_new(void)
{
    sim_mutex *m = calloc(1, sizeof *m);
    if (m) InitializeSRWLock(&m->lock);
    return m;
}
void sim_mutex_lock(sim_mutex *m) { AcquireSRWLockExclusive(&m->lock); }
void sim_mutex_unlock(sim_mutex *m) { ReleaseSRWLockExclusive(&m->lock); }
sim_cond *sim_cond_new(void)
{
    sim_cond *c = calloc(1, sizeof *c);
    if (c) InitializeConditionVariable(&c->cv);
    return c;
}
void sim_cond_wait(sim_cond *c, sim_mutex *m, int timeout_ms)
{
    SleepConditionVariableSRW(&c->cv, &m->lock, timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms, 0);
}
void sim_cond_broadcast(sim_cond *c) { WakeAllConditionVariable(&c->cv); }

typedef struct { sim_thread_fn fn; void *arg; } thread_start;
static DWORD WINAPI thread_entry(LPVOID p)
{
    thread_start ts = *(thread_start *)p;
    free(p);
    ts.fn(ts.arg);
    return 0;
}
int sim_thread_start(sim_thread_fn fn, void *arg)
{
    thread_start *ts = malloc(sizeof *ts);
    HANDLE h;
    if (!ts) return -1;
    ts->fn = fn;
    ts->arg = arg;
    h = CreateThread(NULL, 0, thread_entry, ts, 0, NULL);
    if (!h) { free(ts); return -1; }
    CloseHandle(h);
    return 0;
}

void sim_thread_boost(void) { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL); }

uint64_t sim_now_ns(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * 1e9 / (double)freq.QuadPart);
}

void sim_sleep_ms(int ms) { Sleep(ms < 0 ? 0 : (DWORD)ms); }

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
void sim_sleep_until_ns(uint64_t deadline)
{
#ifdef _MSC_VER
    static __declspec(thread) HANDLE timer;
    static __declspec(thread) int tried;
#else
    static _Thread_local HANDLE timer;
    static _Thread_local int tried;
#endif
    for (;;) {
        uint64_t now = sim_now_ns();
        int64_t left;
        if (now >= deadline) return;
        left = (int64_t)(deadline - now);
        if (left > 1500000) {
            if (!tried) {
                tried = 1;
                timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            }
            if (timer) {
                LARGE_INTEGER due;
                due.QuadPart = -(LONGLONG)((left - 500000) / 100);
                SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
                WaitForSingleObject(timer, INFINITE);
            } else {
                Sleep((DWORD)((left - 1000000) / 1000000));
            }
        } else if (left > 200000) {
            Sleep(0);
        }
    }
}

int sim_net_init(void)
{
    WSADATA w;
    return WSAStartup(MAKEWORD(2, 2), &w) == 0 ? 0 : -1;
}

sim_sock sim_listen_local(int port, char *err, size_t errlen)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in a;
    if (s == INVALID_SOCKET) { snprintf(err, errlen, "socket に失敗しました (%d)", WSAGetLastError()); return SIM_SOCK_INVALID; }
    {
        BOOL excl = TRUE;
        setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&excl, sizeof excl);
    }
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0) {
        snprintf(err, errlen, "127.0.0.1:%d で待ち受けられません（使用中かもしれません。--port で変えてください）", port);
        closesocket(s);
        return SIM_SOCK_INVALID;
    }
    if (listen(s, 16) != 0) { snprintf(err, errlen, "listen に失敗しました"); closesocket(s); return SIM_SOCK_INVALID; }
    return (sim_sock)s;
}

sim_sock sim_accept(sim_sock s)
{
    SOCKET c = accept((SOCKET)s, NULL, NULL);
    return c == INVALID_SOCKET ? SIM_SOCK_INVALID : (sim_sock)c;
}

long sim_recv(sim_sock s, void *buf, size_t n)
{
    int r = recv((SOCKET)s, (char *)buf, (int)(n > 0x7fffffff ? 0x7fffffff : n), 0);
    return r < 0 ? -1 : (long)r;
}

int sim_send_all(sim_sock s, const void *buf, size_t n)
{
    const char *p = buf;
    while (n > 0) {
        int r = send((SOCKET)s, p, (int)(n > 0x10000000 ? 0x10000000 : n), 0);
        if (r <= 0) return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

void sim_close_sock(sim_sock s)
{
    shutdown((SOCKET)s, SD_BOTH);
    closesocket((SOCKET)s);
}

void sim_sock_set_timeout(sim_sock s, int ms)
{
    DWORD t = (DWORD)ms;
    setsockopt((SOCKET)s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&t, sizeof t);
    setsockopt((SOCKET)s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&t, sizeof t);
}

const char *sim_exe_dir(void)
{
    static char dir[MAX_PATH * 4];
    wchar_t wdir[MAX_PATH + 1];
    char *p, *last = NULL, *u;
    DWORD n = GetModuleFileNameW(NULL, wdir, MAX_PATH + 1);
    if (n == 0 || n > MAX_PATH || !(u = sim_win_utf8(wdir))) { strcpy(dir, "."); return dir; }
    if (strlen(u) >= sizeof dir) { free(u); strcpy(dir, "."); return dir; }
    strcpy(dir, u);
    free(u);
    for (p = dir; *p; p++) if (*p == '\\' || *p == '/') last = p;
    if (last) *last = 0; else strcpy(dir, ".");
    return dir;
}

void sim_open_browser(const char *url)
{
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "start \"\" \"%s\"", url);
    system(cmd);
}

void sim_console_utf8(void)
{
    SetConsoleOutputCP(CP_UTF8);
}

#ifdef _MSC_VER
#pragma comment(lib, "shell32.lib")
#endif
#include <shellapi.h>

/* main の argv は ANSI コードページ。コマンドラインをワイド文字で取り直して UTF-8 にする（失敗したら元のまま） */
void sim_utf8_args(int *argc, char ***argv)
{
    int n = 0, i;
    wchar_t **w = CommandLineToArgvW(GetCommandLineW(), &n);
    char **a;
    if (!w) return;
    if (n != *argc || !(a = calloc((size_t)n + 1, sizeof *a))) { LocalFree(w); return; }
    for (i = 0; i < n; i++)
        if (!(a[i] = sim_win_utf8(w[i]))) { LocalFree(w); return; }   /* 途中までの確保はプロセス終了まで残す */
    LocalFree(w);
    *argv = a;
}

/* sim_fopen と同じ規則（UTF-8、読めなければ ANSI）でパスを解釈する。
 * フォルダも開けるよう FILE_FLAG_BACKUP_SEMANTICS、ほかが開いていても開けるよう共有はすべて許す */
static int file_id(const char *path, BY_HANDLE_FILE_INFORMATION *fi)
{
    wchar_t *w = sim_win_wide(path);
    HANDLE h = w ? CreateFileW(w, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS, NULL) : INVALID_HANDLE_VALUE;
    BOOL ok;
    free(w);
    if (h == INVALID_HANDLE_VALUE) return -1;
    ok = GetFileInformationByHandle(h, fi);
    CloseHandle(h);
    return ok ? 0 : -1;
}

int sim_same_file(const char *a, const char *b)
{
    BY_HANDLE_FILE_INFORMATION fa, fb;
    if (file_id(a, &fa) || file_id(b, &fb)) return 0;
    return fa.dwVolumeSerialNumber == fb.dwVolumeSerialNumber && fa.nFileIndexHigh == fb.nFileIndexHigh &&
           fa.nFileIndexLow == fb.nFileIndexLow;
}

#else /* POSIX */

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

struct sim_mutex { pthread_mutex_t m; };
struct sim_cond { pthread_cond_t c; };

sim_mutex *sim_mutex_new(void)
{
    sim_mutex *m = calloc(1, sizeof *m);
    if (m) pthread_mutex_init(&m->m, NULL);
    return m;
}
void sim_mutex_lock(sim_mutex *m) { pthread_mutex_lock(&m->m); }
void sim_mutex_unlock(sim_mutex *m) { pthread_mutex_unlock(&m->m); }
sim_cond *sim_cond_new(void)
{
    sim_cond *c = calloc(1, sizeof *c);
    if (c) pthread_cond_init(&c->c, NULL);
    return c;
}
void sim_cond_wait(sim_cond *c, sim_mutex *m, int timeout_ms)
{
    struct timespec ts;
    struct timeval tv;
    if (timeout_ms < 0) { pthread_cond_wait(&c->c, &m->m); return; }
    gettimeofday(&tv, NULL);
    ts.tv_sec = tv.tv_sec + timeout_ms / 1000;
    ts.tv_nsec = (long)tv.tv_usec * 1000 + (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_cond_timedwait(&c->c, &m->m, &ts);
}
void sim_cond_broadcast(sim_cond *c) { pthread_cond_broadcast(&c->c); }

typedef struct { sim_thread_fn fn; void *arg; } thread_start;
static void *thread_entry(void *p)
{
    thread_start ts = *(thread_start *)p;
    free(p);
    ts.fn(ts.arg);
    return NULL;
}
int sim_thread_start(sim_thread_fn fn, void *arg)
{
    pthread_t t;
    pthread_attr_t at;
    thread_start *ts = malloc(sizeof *ts);
    if (!ts) return -1;
    ts->fn = fn;
    ts->arg = arg;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &at, thread_entry, ts) != 0) { pthread_attr_destroy(&at); free(ts); return -1; }
    pthread_attr_destroy(&at);
    return 0;
}

void sim_thread_boost(void) {}

uint64_t sim_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

void sim_sleep_ms(int ms)
{
    struct timespec ts;
    if (ms < 0) ms = 0;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

void sim_sleep_until_ns(uint64_t deadline)
{
    for (;;) {
        uint64_t now = sim_now_ns();
        uint64_t left;
        struct timespec ts;
        if (now >= deadline) return;
        left = deadline - now;
        ts.tv_sec = (time_t)(left / 1000000000ULL);
        ts.tv_nsec = (long)(left % 1000000000ULL);
        nanosleep(&ts, NULL);
    }
}

int sim_net_init(void)
{
    signal(SIGPIPE, SIG_IGN);
    return 0;
}

sim_sock sim_listen_local(int port, char *err, size_t errlen)
{
    int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    struct sockaddr_in a;
    if (s < 0) { snprintf(err, errlen, "socket に失敗しました: %s", strerror(errno)); return SIM_SOCK_INVALID; }
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    fcntl(s, F_SETFD, FD_CLOEXEC);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0) {
        snprintf(err, errlen, "127.0.0.1:%d で待ち受けられません（%s。--port で変えてください）", port, strerror(errno));
        close(s);
        return SIM_SOCK_INVALID;
    }
    if (listen(s, 16) != 0) { snprintf(err, errlen, "listen に失敗しました: %s", strerror(errno)); close(s); return SIM_SOCK_INVALID; }
    return (sim_sock)s;
}

sim_sock sim_accept(sim_sock s)
{
    int c;
    do { c = accept((int)s, NULL, NULL); } while (c < 0 && errno == EINTR);
    if (c < 0) return SIM_SOCK_INVALID;
#ifdef SO_NOSIGPIPE
    { int one = 1; setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one); }
#endif
    fcntl(c, F_SETFD, FD_CLOEXEC);
    return (sim_sock)c;
}

long sim_recv(sim_sock s, void *buf, size_t n)
{
    ssize_t r;
    do { r = recv((int)s, buf, n, 0); } while (r < 0 && errno == EINTR);
    return (long)r;
}

int sim_send_all(sim_sock s, const void *buf, size_t n)
{
    const char *p = buf;
#ifdef MSG_NOSIGNAL
    int fl = MSG_NOSIGNAL;
#else
    int fl = 0;
#endif
    while (n > 0) {
        ssize_t r = send((int)s, p, n, fl);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

void sim_close_sock(sim_sock s)
{
    shutdown((int)s, SHUT_RDWR);
    close((int)s);
}

void sim_sock_set_timeout(sim_sock s, int ms)
{
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt((int)s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt((int)s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

const char *sim_exe_dir(void)
{
    static char dir[4096];
    char *last;
#ifdef __APPLE__
    uint32_t sz = sizeof dir;
    char tmp[4096];
    if (_NSGetExecutablePath(tmp, &sz) != 0) { strcpy(dir, "."); return dir; }
    if (!realpath(tmp, dir)) { strncpy(dir, tmp, sizeof dir - 1); dir[sizeof dir - 1] = 0; }
#else
    ssize_t n = readlink("/proc/self/exe", dir, sizeof dir - 1);
    if (n <= 0) { strcpy(dir, "."); return dir; }
    dir[n] = 0;
#endif
    last = strrchr(dir, '/');
    if (last) *last = 0; else strcpy(dir, ".");
    return dir;
}

void sim_open_browser(const char *url)
{
    pid_t p = fork();
    if (p == 0) {
        int fd = open("/dev/null", O_WRONLY);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); }
#ifdef __APPLE__
        execlp("open", "open", url, (char *)NULL);
#else
        execlp("xdg-open", "xdg-open", url, (char *)NULL);
#endif
        _exit(127);
    }
}

void sim_console_utf8(void) {}
void sim_utf8_args(int *argc, char ***argv) { (void)argc; (void)argv; }

int sim_same_file(const char *a, const char *b)
{
    struct stat sa, sb;
    if (stat(a, &sa) != 0 || stat(b, &sb) != 0) return 0;
    return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

#endif

#ifndef _WIN32
#include <dirent.h>
#endif
int sim_path_kind(const char *path)
{
#ifdef _WIN32
    wchar_t *w = sim_win_wide(path);
    DWORD a = w ? GetFileAttributesW(w) : INVALID_FILE_ATTRIBUTES;
    free(w);
    if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_REPARSE_POINT)) return 0;
    return (a & FILE_ATTRIBUTE_DIRECTORY) ? 2 : 1;
#else
    struct stat st;
    if (lstat(path, &st)) return 0;
    return S_ISDIR(st.st_mode) ? 2 : S_ISREG(st.st_mode) ? 1 : 0;
#endif
}
int sim_dir_each(const char *path, void (*visit)(void *, const char *, int), void *ctx)
{
#ifdef _WIN32
    WIN32_FIND_DATAW data;
    size_t n = strlen(path);
    char *pattern = malloc(n + 3);
    wchar_t *wpattern;
    HANDLE h;
    if (!pattern) return -1;
    memcpy(pattern, path, n); memcpy(pattern + n, "\\*", 3);
    wpattern = sim_win_wide(pattern); free(pattern);
    if (!wpattern) return -1;
    h = FindFirstFileW(wpattern, &data); free(wpattern);
    if (h == INVALID_HANDLE_VALUE) return -1;
    do {
        char *name;
        if (!wcscmp(data.cFileName, L".") || !wcscmp(data.cFileName, L"..") ||
            (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        name = sim_win_utf8(data.cFileName);   /* 名前は UTF-8 で渡す（プログラム内のパスは UTF-8） */
        if (!name) continue;
        visit(ctx, name, (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 2 : 1);
        free(name);
    } while (FindNextFileW(h, &data));
    { DWORD e = GetLastError(); FindClose(h); return e == ERROR_NO_MORE_FILES ? 0 : -1; }
#else
    struct dirent *de;
    DIR *dir = opendir(path);
    if (!dir) return -1;
    while ((de = readdir(dir)) != NULL) {
        char *p;
        size_t a, b;
        int kind;
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        a = strlen(path); b = strlen(de->d_name);
        p = malloc(a + b + 2);
        if (!p) { closedir(dir); return -1; }
        memcpy(p, path, a); p[a] = '/'; memcpy(p + a + 1, de->d_name, b + 1);
        kind = sim_path_kind(p); free(p);
        if (kind) visit(ctx, de->d_name, kind);
    }
    closedir(dir); return 0;
#endif
}
