/* panel.c —— hphelm 面板 + 自研锁屏（状态机）
 *
 * 用户要的四条行为：
 *   ① 长时间未操作 → 进入自己画的资源面板
 *   ② 晃鼠标 / 点一下 → 进入登录页
 *   ③ 登录成功 → 回到桌面正常操作
 *   ④ 再次长时间未操作 → 重新进入面板（循环）
 *
 * 技术选型（实测，详见 README 与 runbooks/hphelm.md）：
 *   X11 + cairo + pango，**不套 GUI 工具包**；最小程序 PSS 7.4 MiB / 2 线程。
 *   认证走自建 setuid 助手 hpopi5b-auth（PAM `login` 栈），**不再用 lightdm 的 greeter**。
 *
 * 状态机：
 *   DESKTOP ──idle≥panel_idle──▶ PANEL ──input(≥input_grace)──▶ LOGIN
 *      ▲                                                          │
 *      └──────────────认证成功──────────────────────────────────┘
 *   LOGIN ──idle≥login_back──▶ PANEL（密码框留空、清错误）
 *   PANEL ──idle≥sleep_off──▶ 关屏（DPMS），任意输入即唤醒
 *        └─ sleep_off ≤ 0 ⇒ 永不关屏（面板要 7×24 一直显示，2026-10-19 口径）
 *
 * 多幕（scene）：面板本身只是「一幕」。--kiosk 下默认 30s 轮换一幕，
 *   固定特效循环；每幕自带内容签名，与旧版一样「内容不变不重绘」。
 *   转场用两块离屏缓冲（失败才分配）做切片/淡出，不直接画窗口。
 *
 * 用法： hpopi5b-panel [--scale N] [--panel-idle S] [--login-back S]
 *                     [--input-grace S] [--sleep-off S] [--dump] [--once]
 *                     [--remote HOST]  （生产服务器：ssh 目标；空/缺省 = 不采集）
 *                     [--scene ID]     （钉住一幕，不轮换：panel|trending）
 *                     [--rotate S]     （轮换间隔，秒；0 = 不轮换）
 *                     [--transition FX] （cut|dip|push|cover；缺省每轮换一种）
 *                     [--list-scenes]  （列出幕的 id 与名字后退出）
 *                     [--state desktop|panel|login]   （测试用：强制初始状态）
 *                     [--lock PATH]                   （单实例锁路径，默认 /tmp/hpopi5b-panel.lock）
 *                     [--theme auto|day|night]        （auto = 按固定时刻自动切；默认 auto）
 *                     [--theme-day HH:MM]             （白天起点，默认 07:00）
 *                     [--theme-night HH:MM]           （夜间起点，默认 19:00）
 */
#define _GNU_SOURCE
#include <X11/XKBlib.h>
#include <X11/extensions/scrnsaver.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <cairo/cairo-xlib.h>
#include <cairo/cairo.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "auth.h"
#include "metrics.h"
#include "remote.h"
#include "scene.h"
#include "theme.h"
#include "view.h"

enum { ST_DESKTOP, ST_PANEL, ST_LOGIN };

static const char *state_name(int s) {
    switch (s) {
    case ST_DESKTOP: return "DESKTOP";
    case ST_PANEL: return "PANEL";
    case ST_LOGIN: return "LOGIN";
    default: return "?";
    }
}

/* ---------------- 配置 ---------------- */
static double SCALE = 1.48;
static double CFG_PANEL_IDLE = 180;   /* 桌面空闲多久进面板 */
static double CFG_LOGIN_BACK = 120;   /* 登录页空闲多久回面板 */
static double CFG_INPUT_GRACE = 1.2;  /* 进面板后多久才接受“唤醒”（防误触） */
static double CFG_SLEEP_OFF = 0;      /* 面板空闲多久关屏；**0 或负数 = 永不关屏**（当前默认） */
static char CFG_REMOTE[64] = "";      /* 生产服务器 ssh 目标；空 = 不采集（默认） */
/* 配置文件：**改主机名不该需要编辑 root 的会话包装器**（那是 /usr/local/bin 下的 root 文件，
 * 而且改了还得重启整个会话才生效）。优先级：--remote > HPOPI5B_REMOTE > 本文件。
 * 格式：一行一个 key=value，`#` 注释，只认 remote=。 */
static const char *CFG_FILE = "/etc/hpopi5b-panel.conf";
static int FORCE_STATE = -1;
static int CFG_KIOSK = 0;  /* --kiosk：纯显示器（永不进登录页/桌面，输入一律忽略） */
static char CFG_REMOTE2_DEAD[64] = ""; /* 已废弃的 remote2=（只用来提示「配了但不生效」） */
/* 幕轮换：-1 = 未指定 ⇒ kiosk 默认 30s，桌面/锁屏默认不轮换（要显式打开） */
static double CFG_ROTATE = -1;
static char CFG_SCENE[32] = "";    /* 钉住某一幕（--scene）；空 = 从 panel 开始轮换 */
static int CFG_TRANS = -1;          /* -1 = 每次自动换一种特效；>=0 = 固定 */
static char CFG_TRENDING[256];
static char CFG_NEWS[256];         /* 新闻幕缓存文件（news=）；空 = 用 scene 默认路径 */
static int CFG_THEME = -1;             /* -1 自动（按时刻）· 0 白天 · 1 夜间 */
static int CFG_THEME_DAY = 7 * 60;     /* 白天起点（分钟） */
static int CFG_THEME_NIGHT = 19 * 60;  /* 夜间起点（分钟） */

static int theme_night;                /* 当前生效的主题 */
static int theme_fade;                 /* 1 = 正在「淡出→换色→淡入」 */
static double theme_fade_t0;
static int theme_fade_pending, theme_fade_swapped;
static double theme_check_at;
#define THEME_FADE 0.45                /* 单向淡变时长（半程换色） */
      /* GitHub 趋势缓存文件（trending=）；空 = 用 scene 默认路径 */
static int DBG = 0;               /* HPOPI5B_PANEL_DEBUG=1：每秒打印主循环仪表 */
static long dbg_iter, dbg_ev, dbg_poll_rd, dbg_poll_to, dbg_poll_oth, dbg_redraw, dbg_sample;
static long dbg_remote_frames;
static long dbg_evtype[64];
static double dbg_ms_render, dbg_ms_blit, dbg_ms_sig;
static double dbg_ms_samp, dbg_ms_rd;   /* 采样 / 远端解析的耗时（定位 CPU 花在哪儿） */

static const char *CFG_LOCK = "/tmp/hpopi5b-panel.lock";  /* 单实例锁（--lock 可换，便于 Xvfb 并行测试） */

/* 读配置文件里的 remote=（找不到文件就静默跳过） */
/* "07:30" → 450（分钟）；格式不对返回 -1 */
static int parse_hhmm(const char *s) {
    int h = -1, m = -1;
    if (sscanf(s, "%d:%d", &h, &m) != 2) return -1;
    if (h < 0 || h > 23 || m < 0 || m > 59) return -1;
    return h * 60 + m;
}

static void read_conf_file(void) {
    FILE *f = fopen(CFG_FILE, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || !*p) continue;
        if (strncmp(p, "remote=", 7) == 0 && !CFG_REMOTE[0]) {
            char *v = p + 7;
            v[strcspn(v, " \t\r\n")] = 0;
            if (v[0]) snprintf(CFG_REMOTE, sizeof CFG_REMOTE, "%s", v);
            continue;
        }
        /* remote2= 已废弃（2026-11-06 单机化：hpcmd 下线，全场只剩 hpcore）。
         * 老 conf 还会带这行 ⇒ 只提示一次，不报错、不影响启动。 */
        if (strncmp(p, "remote2=", 8) == 0) {
            char *v = p + 8;
            v[strcspn(v, " \t\r\n")] = 0;
            if (v[0] && !CFG_REMOTE2_DEAD[0]) snprintf(CFG_REMOTE2_DEAD, sizeof CFG_REMOTE2_DEAD, "%s", v);
            continue;
        }
        if (strncmp(p, "kiosk=", 6) == 0) {
            char *v = p + 6;
            v[strcspn(v, " \t\r\n")] = 0;
            CFG_KIOSK = (v[0] == '1' || v[0] == 'y' || v[0] == 'Y' || v[0] == 't');
            continue;
        }
        if (strncmp(p, "rotate=", 7) == 0 && CFG_ROTATE < 0) {
            char *v = p + 7;
            v[strcspn(v, " \t\r\n")] = 0;
            CFG_ROTATE = atof(v);
            continue;
        }
        if (strncmp(p, "scene=", 6) == 0 && !CFG_SCENE[0]) {
            char *v = p + 6;
            v[strcspn(v, " \t\r\n")] = 0;
            if (v[0]) snprintf(CFG_SCENE, sizeof CFG_SCENE, "%s", v);
            continue;
        }
        if (strncmp(p, "transition=", 11) == 0 && CFG_TRANS < 0) {
            char *v = p + 11;
            v[strcspn(v, " \t\r\n")] = 0;
            int fx = scene_fx_by_name(v);
            if (fx >= 0) CFG_TRANS = fx;
            continue;
        }
        if (strncmp(p, "theme=", 6) == 0) {
            char *v = p + 6;
            v[strcspn(v, " \t\r\n")] = 0;
            if (!strcmp(v, "day")) CFG_THEME = 0;
            else if (!strcmp(v, "night")) CFG_THEME = 1;
            else if (!strcmp(v, "auto")) CFG_THEME = -1;
            continue;
        }
        if (strncmp(p, "theme_day=", 10) == 0) {
            char *v = p + 10;
            v[strcspn(v, " \t\r\n")] = 0;
            int m = parse_hhmm(v);
            if (m >= 0) CFG_THEME_DAY = m;
            continue;
        }
        if (strncmp(p, "theme_night=", 12) == 0) {
            char *v = p + 12;
            v[strcspn(v, " \t\r\n")] = 0;
            int m = parse_hhmm(v);
            if (m >= 0) CFG_THEME_NIGHT = m;
            continue;
        }
        if (strncmp(p, "trending=", 9) == 0 && !CFG_TRENDING[0]) {
            char *v = p + 9;
            v[strcspn(v, " \t\r\n")] = 0;
            if (v[0]) snprintf(CFG_TRENDING, sizeof CFG_TRENDING, "%s", v);
            continue;
        }
        if (strncmp(p, "news=", 5) == 0 && !CFG_NEWS[0]) {
            char *v = p + 5;
            v[strcspn(v, " \t\r\n")] = 0;
            if (v[0]) snprintf(CFG_NEWS, sizeof CFG_NEWS, "%s", v);
            continue;
        }
    }
    fclose(f);
}

/* 关屏阈值的人类可读形式（0 ⇒ 永不关屏），日志/--dump 共用 */
static void sleep_off_desc(char *buf, size_t n) {
    if (CFG_SLEEP_OFF > 0) snprintf(buf, n, "%.0fs", CFG_SLEEP_OFF);
    else snprintf(buf, n, "off（永不关屏）");
}

/* ---------------- X 状态 ---------------- */
static Display *dpy;
static Window root, win;
static int SW = 1920, SH = 1080;
static cairo_surface_t *surf;
static cairo_t *cr;
static PangoLayout *lay;
static metrics_t M;              /* 本机 */
static remote_t R;               /* 生产服务器（--remote；未配置时 enabled=0） */
static view_feed_t FEED;
static int scr;
static Pixmap back;                /* 离屏后备缓冲（X 服务器端 pixmap，双缓冲用） */
static Pixmap bgpix;               /* 渐变底（整屏画一次，每帧 XCopyArea 复制过来） */
static GC blitgc;                  /* 一次 XCopyArea 上屏用的 GC */
static unsigned long long g_sig;   /* 上一帧内容签名（内容没变就不重绘） */
static int g_sig_valid;
/* ---- 幕与转场 ---- */
static scene_env_t ENV;            /* 每帧重填（指针指向 M/R/FEED） */

/* ---------------- 运行状态 ---------------- */
static int state = ST_DESKTOP;
static double state_enter = 0;
static char g_user[64];
static char pw[256];
static int pwlen = 0;
static char g_err[128];
static int busy = 0;
static int retry_at = 0;      /* 失败冷却结束的时刻（monotonic） */
static int fail_count = 0;    /* 连续失败次数（用于指数退避） */
static int screen_off = 0;
static int cursor_on = 1;

static volatile sig_atomic_t g_sig_lock = 0;
static volatile sig_atomic_t g_sig_panel = 0;
static void on_sig(int sig) {
    if (sig == SIGUSR1) g_sig_lock = 1;
    else if (sig == SIGUSR2) g_sig_panel = 1;
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static double x_idle(void) {
    XScreenSaverInfo *info = XScreenSaverAllocInfo();
    if (!info) return 0;
    double idle = 0;
    if (XScreenSaverQueryInfo(dpy, root, info)) idle = info->idle / 1000.0;
    XFree(info);
    return idle;
}

static void logf_(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char t[32];
    time_t tt = time(NULL);
    strftime(t, sizeof t, "%H:%M:%S", localtime(&tt));
    printf("[%s] ", t);
    vprintf(fmt, ap);
    printf("\n");
    fflush(stdout);
    va_end(ap);
}

/* ---------------- 双缓冲（离屏后备 pixmap） ----------------
 * ⚠️ 为什么不直接画到窗口：实测直接画窗口时，一帧里「清底 → 逐块绘制」的中间态
 *    会被合成器（picom）与屏幕看到 ⇒ 每帧 63% 像素变化、每秒闪两次
 *    （探针证据：能抓到「只有底色、没有盒子/文字」的半成品帧）。
 *    现在所有绘制都进离屏 pixmap，最后用一次 XCopyArea 上屏
 *    ⇒ 屏幕要么看到旧帧、要么看到新帧，没有中间态。
 *    顺带把这块 pixmap 设为窗口背景 ⇒ 暴露/映射时由服务器自己填，不会闪黑。
 */
static void render(void);
static void blit(void);
static void redraw(void);
static void redraw_force(void);

/* 背景 pixmap：整屏渐变只画一次。
 * ⚠️ 实测每帧重画整屏渐变要 4~5ms（cairo 的 pattern 在 Xlib 表面上会掉进客户端渲染），
 *    1 Hz 就是白吃 0.5% of one core。改成「画一次 + 每帧服务器端 XCopyArea」后
 *    这一段降到几乎为 0（和上屏用的 blit 同一条路径）。 */
static void bg_init(void) {
    if (bgpix) { XFreePixmap(dpy, bgpix); bgpix = 0; }
    bgpix = XCreatePixmap(dpy, root, (unsigned)SW, (unsigned)SH,
                          (unsigned)DefaultDepth(dpy, scr));
    cairo_surface_t *bs = cairo_xlib_surface_create(dpy, bgpix, DefaultVisual(dpy, scr), SW, SH);
    cairo_t *bcr = cairo_create(bs);
    cairo_scale(bcr, SCALE, SCALE);
    v_bg(bcr, SW / SCALE, SH / SCALE);
    cairo_destroy(bcr);
    cairo_surface_flush(bs);
    cairo_surface_destroy(bs);
}

/* 换主题 = 换调色板 + 重画一次整屏渐变（bgpix）+ 让签名失效。
   ⚠️ 必须重画 bgpix：它里面烤的是旧主题的渐变，只换宏不会变。 */
static void theme_apply(int night) {
    theme_night = night ? 1 : 0;
    theme_set(theme_night);
    if (dpy) bg_init();
    g_sig_valid = 0;
    logf_("主题：%s", theme_night ? "夜间" : "白天");
}

/* 淡变遮罩：前半程 0→1（淡出到黑），中途换色，后半程 1→0（淡入） */
static void theme_fade_paint(cairo_t *c) {
    if (!theme_fade) return;
    double t = now_s() - theme_fade_t0;
    double a = t < THEME_FADE ? t / THEME_FADE : 1.0 - (t - THEME_FADE) / THEME_FADE;
    if (a < 0) a = 0;
    if (a > 1) a = 1;
    cairo_save(c);
    cairo_set_source_rgba(c, 0, 0, 0, a);
    cairo_paint(c);
    cairo_restore(c);
}

static void surface_init(void) {
    if (cr) { cairo_destroy(cr); cr = NULL; }
    if (surf) { cairo_surface_destroy(surf); surf = NULL; }
    if (back) { XFreePixmap(dpy, back); back = 0; }
    back = XCreatePixmap(dpy, root, (unsigned)SW, (unsigned)SH,
                         (unsigned)DefaultDepth(dpy, scr));
    /* 尺寸变了 ⇒ 转场缓冲跟着重建（若已分配；重建会打断正在进行的转场，可接受） */
    bg_init();
    surf = cairo_xlib_surface_create(dpy, back, DefaultVisual(dpy, scr), SW, SH);
    cr = cairo_create(surf);
    lay = pango_cairo_create_layout(cr);
    view_init(lay);
    XSetWindowBackgroundPixmap(dpy, win, back);
}

/* ---------------- 窗口显隐 ---------------- */
/* ⚠️ 键盘 grab 必须「显示时抓、隐藏时放」：
 *    只在启动时抓一次 ⇒ 回到 DESKTOP 后 grab 仍生效 ⇒ 桌面收不到任何按键（实测会踩）。
 *    隐藏时还要把焦点还给 root，否则焦点留在一个未映射窗口上。 */
static void win_raise(void) { XRaiseWindow(dpy, win); XFlush(dpy); }

static void win_show(void) {
    /* 先把这一帧画进后备缓冲，再映射 ⇒ 不会先露出空白/黑窗口 */
    g_sig_valid = 0;
    render();
    XMapRaised(dpy, win);
    XSync(dpy, False);
    blit();
    XSetInputFocus(dpy, win, RevertToPointerRoot, CurrentTime);
    XGrabKeyboard(dpy, win, True, GrabModeAsync, GrabModeAsync, CurrentTime);
    XFlush(dpy);
}

static void win_hide(void) {
    XUngrabKeyboard(dpy, CurrentTime);
    XSetInputFocus(dpy, PointerRoot, RevertToPointerRoot, CurrentTime);
    XUnmapWindow(dpy, win);
    XFlush(dpy);
}

static void set_state(int s) {
    /* ⭐ kiosk：永不离开面板。把「谁在叫切状态」的 4 个调用点（按键/移动/点击/SIGUSR1）
     *   全部收口在这里 —— 逐个加守卫是 bug 温床（漏一个就冒登录页）。 */
    if (CFG_KIOSK && s != ST_PANEL) return;
    if (s == state) return;
    logf_("状态 %s → %s（idle=%.1fs）", state_name(state), state_name(s), x_idle());
    state = s;
    state_enter = now_s();
    if (s == ST_PANEL) scenes_resume(state_enter); /* 打断转场 + 停留计时重来 */
    g_err[0] = 0;
    if (s == ST_DESKTOP) {
        win_hide();
        pwlen = 0;
        pw[0] = 0;
    } else {
        win_show();
    }
    if (s == ST_LOGIN) {
        pwlen = 0;
        pw[0] = 0;
        retry_at = 0;
    }
}

/* 关屏 / 唤醒 */
static void dpms(int on) {
    /* on=1 开屏；on=0 关屏 */
    Display *d = dpy;
    char cmd[192];
    if (on && CFG_SLEEP_OFF <= 0) {
        /* 「永不关屏」不只是「本程序不去关」：X server 侧有它自己的屏保与 DPMS 定时器。
         * 2026-11-03 实测（hpopi5b kiosk，无键鼠）：X 默认 `Screen Saver timeout=600s` +
         * `DPMS Standby/Suspend/Off=600s` ⇒ 开机 10 分钟后显示器自己黑屏。
         * 而且**单单 `xset -dpms` 不够**：本次这行 `xset dpms force on` 会把 DPMS 重新启用。
         * 所以每次点亮都把策略重新钉一遍，并且**把三个超时设成 0**（0 = 该超时被禁用，
         * 实测之后即使再来 `force on` 或被 `+dpms` 启用，超时仍然是 0）。 */
        snprintf(cmd, sizeof cmd,
                 "xset s off; xset s noblank; xset dpms force on; xset dpms 0 0 0; xset -dpms");
    } else {
        snprintf(cmd, sizeof cmd, "xset dpms force %s", on ? "on" : "off");
    }
    if (system(cmd) != 0) { /* 忽略 */ }
    screen_off = !on;
    (void)d;
}

/* ---------------- 绘制 ---------------- */
/* 幕的数据环境：每帧重填（now/wall 会变），指针常量指向采样结构 */
static void env_fill(void) {
    ENV.now = now_s();
    ENV.wall = time(NULL);
    ENV.m = &M;
    ENV.r = &R;
    ENV.feed = &FEED;
}

/* 把一幕画进指定缓冲：先服务器端拷底图，再 cairo 画内容。pm/cr/surf 必须配对。 */
static void scene_into(Pixmap pm, cairo_t *tcr, cairo_surface_t *tsurf, const scene_t *s) {
    cairo_surface_flush(tsurf);
    XCopyArea(dpy, bgpix, pm, blitgc, 0, 0, (unsigned)SW, (unsigned)SH, 0, 0);
    cairo_surface_mark_dirty(tsurf);
    view_ctx_t v = {.scale = SCALE, .lw = SW / SCALE, .lh = SH / SCALE, .bg_done = 1};
    cairo_save(tcr);
    cairo_scale(tcr, SCALE, SCALE);
    s->draw(tcr, &v, &ENV);
    cairo_restore(tcr);
    cairo_surface_flush(tsurf);
}

/* 底部居中的幕指示：几个小点，当前幕实心。轮换关掉时不画。 */
static void draw_hud(cairo_t *tcr) {
    int n = scenes_count();
    if (n < 2 || !scenes_rotating()) return;
    int cur = scenes_current_index();
    if (scenes_transitioning()) cur = -1;
    double cx = (SW / SCALE) / 2, y = (SH / SCALE) - 9;
    for (int i = 0; i < n; i++) {
        double x = cx + (i - (n - 1) / 2.0) * 14.0;
        v_set_rgb(tcr, i == cur ? C_SUBTEXT1 : C_OVERLAY0, i == cur ? 0.9 : 0.35);
        cairo_arc(tcr, x, y, i == cur ? 2.4 : 1.8, 0, 2 * M_PI);
        cairo_fill(tcr);
    }
}

/* 转场：只有「淡出到背景色再淡入」一种（硬切用于测试）。
 * 为什么不做平移：平移要把两幕分别画进两块 8 MiB 缓冲再做切片，
 * 视觉收益小、每次切幕多一次全屏绘制；淡变只需要一块缓冲 + 一次纯色覆盖。 */
static void render_transition(scene_trans_t *T) {
    double t = (ENV.now - T->t0) / T->dur;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    switch (T->fx) {
    case FX_DIP: {
        scene_into(back, cr, surf, t < 0.5 ? T->from : T->to);
        double a = t < 0.5 ? t * 2 : (1 - t) * 2; /* 0 → 1 → 0 */
        cairo_save(cr);
        cairo_scale(cr, SCALE, SCALE);
        v_set_rgb(cr, C_CRUST, a);
        cairo_paint(cr);
        cairo_restore(cr);
        cairo_surface_flush(surf);
    } break;
    case FX_CUT:
    default:
        scene_into(back, cr, surf, T->to);
        break;
    }
}

/* 内容签名：只有它变了才重画（时钟每秒变 ⇒ 面板仍是 1Hz；
   空闲无变化时不再白白重绘，省 CPU 也少一次上屏） */
static unsigned long long content_sig(void) {
    unsigned long long h = 1469598103934665603ULL; /* FNV-1a 64 */
    const scene_t *sc = (state == ST_PANEL) ? scenes_current() : NULL;
    /* 1Hz 采样快照只有「吃实时指标」的幕用得上（面板）和登录页用得上。
     * ⚠ 新闻/科技幕只吃外部文件：它们的 sig 自己按分钟变，
     * 把每秒都会变的 M / R.m 无条件混进来 = 每幕每秒整屏重绘（实测 ~20ms/次）。 */
    int live = (state != ST_PANEL) || !sc || sc->live || scenes_transitioning();
    if (live) {
        const unsigned char *p = (const unsigned char *)&M;
        for (size_t i = 0; i < sizeof M; i++) { h ^= p[i]; h *= 1099511628211ULL; }
        /* 远端的快照也要进签名：否则「本机没变、远端变了」的秒里画面不更新 */
        if (R.enabled) {
            p = (const unsigned char *)&R.m;
            for (size_t i = 0; i < sizeof R.m; i++) { h ^= p[i]; h *= 1099511628211ULL; }
        }
    }
#define MIX(v) do { h ^= (unsigned long long)(v); h *= 1099511628211ULL; } while (0)
    MIX(state);
    MIX(pwlen);
    MIX(busy);
    MIX(cursor_on);
    MIX(FEED.online);
    /* 离线秒数按**整秒**入签名：远端一断，屏幕要能持续走「离线 Ns」而不是冻住。
     * ⚠️ 只在**离线**时入：在线时 age_s 每秒都在涨（它 = now - last_frame），
     * 把它无条件混进签名会让**每一幕**每秒整屏重绘 —— 新闻/科技幕的数据按分钟才变，
     * 那是白烧 CPU（实测 news 幕绘制 18ms ⇒ 每秒重绘 ≈ 1.8% 单核）。
     * 面板幕自己的数据是 1Hz 的，它会在 scene_panel.c 的 sig 里自己混。 */
    MIX(FEED.age_s >= 0 && !FEED.online ? (long)FEED.age_s : -1L);
    MIX(retry_at > 0 ? retry_at - (int)now_s() : 0);
    for (const unsigned char *e = (const unsigned char *)g_err; *e; e++) {
        h ^= *e;
        h *= 1099511628211ULL;
    }
    if (state == ST_PANEL) {
        env_fill();
        /* 幕内容也进签名：换幕 / 转场进度 / 幕内数据变化才重绘 */
        if (sc->sig) MIX(sc->sig(&ENV));
        MIX(scenes_current_index());
        if (scenes_transitioning()) {
            MIX(scenes_transition()->gen);
            MIX((long)((ENV.now - scenes_transition()->t0) * 100));
        }
    }
#undef MIX
    return h;
}

/* 把当前状态画进离屏后备缓冲（不碰窗口） */
static void render(void) {
    if (state == ST_DESKTOP) return;
    scene_trans_t *T = scenes_transition();
    if (state == ST_PANEL) {
        env_fill();
        if (T->active) render_transition(T);
        else scene_into(back, cr, surf, scenes_current());
        cairo_save(cr);
        cairo_scale(cr, SCALE, SCALE);
        draw_hud(cr);
        cairo_restore(cr);
        theme_fade_paint(cr);
        cairo_surface_flush(surf);
        return;
    }
    /* 先铺底（服务器端 blit，不经过像素），再让 cairo 在最上面画内容 */
    if (bgpix) {
        cairo_surface_flush(surf);
        XCopyArea(dpy, bgpix, back, blitgc, 0, 0, (unsigned)SW, (unsigned)SH, 0, 0);
        cairo_surface_mark_dirty(surf);
    }
    view_ctx_t v = {.scale = SCALE, .lw = SW / SCALE, .lh = SH / SCALE, .bg_done = bgpix != 0};
    cairo_save(cr);
    cairo_scale(cr, SCALE, SCALE);
    int wait = (retry_at > 0) ? (int)(retry_at - now_s()) : 0;
    if (wait < 0) wait = 0;
    view_login_draw(cr, &v, &M, g_user, pwlen, g_err, busy, cursor_on, wait);
    cairo_restore(cr);
    theme_fade_paint(cr);
}

/* 后备缓冲 → 窗口：整屏一次 XCopyArea（服务器端 blit，不上传像素） */
static void blit(void) {
    cairo_surface_flush(surf);
    XCopyArea(dpy, back, win, blitgc, 0, 0, (unsigned)SW, (unsigned)SH, 0, 0);
    XFlush(dpy);
}

static void redraw(void) {
    if (state == ST_DESKTOP) return;
    if (theme_fade) g_sig_valid = 0;   /* 淡变期间每帧都要重画（遮罩透明度在变） */
    double t0 = now_s();
    unsigned long long s = content_sig();
    double t1 = now_s();
    if (g_sig_valid && s == g_sig) return;
    g_sig = s;
    g_sig_valid = 1;
    dbg_redraw++;
    render();
    double t2 = now_s();
    blit();
    double t3 = now_s();
    dbg_ms_sig += (t1 - t0) * 1000; dbg_ms_render += (t2 - t1) * 1000; dbg_ms_blit += (t3 - t2) * 1000;
}

/* 内容没变也要重画（窗口暴露、尺寸变化、刚映射时用） */
static void redraw_force(void) {
    g_sig_valid = 0;
    redraw();
}

/* ---------------- 认证 ---------------- */
static void try_auth(void) {
    if (busy) return;
    if (retry_at > 0 && now_s() < retry_at) return;
    if (pwlen == 0) return;
    busy = 1;
    g_err[0] = 0;
    redraw();
    int r = auth_check(g_user, pw);
    busy = 0;
    if (r == 1) {
        logf_("认证成功 ⇒ 回桌面");
        pwlen = 0;
        pw[0] = 0;
        fail_count = 0;
        set_state(ST_DESKTOP);
        /* 把 idle 归零：刚解锁不该马上又进面板 */
        XResetScreenSaver(dpy);
        XFlush(dpy);
    } else {
        pwlen = 0;
        pw[0] = 0;
        fail_count++;
        int wait = 2 << (fail_count > 4 ? 4 : fail_count - 1); /* 2,4,8,16,32 秒 */
        if (wait > 32) wait = 32;
        retry_at = (int)now_s() + wait;
        snprintf(g_err, sizeof g_err, "%s", r < 0 ? "无法启动认证助手" : "密码错误");
        logf_("认证失败（第 %d 次，冷却 %d 秒）", fail_count, wait);
        redraw();
    }
}

/* ---------------- 输入处理 ---------------- */
static void on_key(XKeyEvent *ke) {
    /* 面板态：任意键都算“唤醒” */
    if (state == ST_PANEL) {
        if (now_s() - state_enter >= CFG_INPUT_GRACE) {
            if (screen_off) dpms(1);
            set_state(ST_LOGIN);
        }
        return;
    }
    if (state != ST_LOGIN) return;
    if (screen_off) {
        dpms(1);
        return;
    }
    KeySym ks = XkbKeycodeToKeysym(dpy, (KeyCode)ke->keycode, 0, 0);
    if (ks == XK_Return || ks == XK_KP_Enter) {
        try_auth();
        return;
    }
    if (ks == XK_Escape) {
        pwlen = 0;
        pw[0] = 0;
        g_err[0] = 0;
        redraw();
        return;
    }
    if (ks == XK_BackSpace) {
        if (pwlen > 0) pw[--pwlen] = 0;
        g_err[0] = 0;
        redraw();
        return;
    }
    /* 可打印字符 */
    char buf[8] = {0};
    int n = XLookupString(ke, buf, sizeof buf - 1, &ks, NULL);
    if (n > 0 && pwlen + n < (int)sizeof pw) {
        memcpy(pw + pwlen, buf, (size_t)n);
        pwlen += n;
        pw[pwlen] = 0;
        g_err[0] = 0;
        redraw();
    }
}

static void on_motion(void) {
    if (state == ST_PANEL) {
        if (now_s() - state_enter >= CFG_INPUT_GRACE) {
            if (screen_off) dpms(1);
            set_state(ST_LOGIN);
        }
        return;
    }
    if (state == ST_LOGIN && screen_off) dpms(1);
}

static void on_button(void) {
    if (state == ST_PANEL) {
        if (now_s() - state_enter >= CFG_INPUT_GRACE) {
            if (screen_off) dpms(1);
            set_state(ST_LOGIN);
        }
        return;
    }
    if (state == ST_LOGIN && screen_off) dpms(1);
}

/* ---------------- 主循环仪表（排障用） ---------------- */
static void dbg_tick(void) {
    if (!DBG) return;
    static double t_last;
    double t = now_s();
    if (t_last == 0) { t_last = t; return; }
    if (t - t_last < 1.0) return;
    char buf[512];
    int n = snprintf(buf, sizeof buf, "主循环 %.0f it/s · 事件 %ld · poll[可读 %ld/超时 %ld/其他 %ld] · "
                     "重绘 %ld · 采样 %ld · 远端帧 %ld · 类型:", dbg_iter / (t - t_last), dbg_ev,
                     dbg_poll_rd, dbg_poll_to, dbg_poll_oth, dbg_redraw, dbg_sample,
                     dbg_remote_frames);
    static const char *tn[] = {"?", "?", "KeyPress", "KeyRelease", "ButtonPress", "ButtonRelease",
                               "MotionNotify", "EnterNotify", "LeaveNotify", "FocusIn", "FocusOut",
                               "KeymapNotify", "Expose", "GraphicsExpose", "NoExpose",
                               "VisibilityNotify", "CreateNotify", "DestroyNotify", "UnmapNotify",
                               "MapNotify", "MapRequest", "ReparentNotify", "ConfigureNotify",
                               "ConfigureRequest", "GravityNotify", "ResizeRequest",
                               "CirculateNotify", "CirculateRequest", "PropertyNotify",
                               "SelectionClear", "SelectionRequest", "SelectionNotify",
                               "ColormapNotify", "ClientMessage", "MappingNotify", "GenericEvent"};
    for (int i = 0; i < 64; i++)
        if (dbg_evtype[i])
            n += snprintf(buf + n, sizeof buf - (size_t)n, " %s=%ld",
                          i < (int)(sizeof tn / sizeof tn[0]) ? tn[i] : "?", dbg_evtype[i]);
    {
        /* 曲线健康度：NaN 会被 cairo 静默丢弃（整段不画），所以必须显式数出来 */
        const struct { const char *n; const ring_t *r; } rings[] = {
            {"cpu", &M.cpu_hist}, {"mem", &M.mem_hist}, {"rx", &M.rx_hist}, {"tx", &M.tx_hist}};
        char rb[256];
        int off = 0;
        for (unsigned i = 0; i < sizeof rings / sizeof rings[0]; i++) {
            int nan = 0;
            for (int k = 0; k < rings[i].r->len; k++)
                if (!(ring_at(rings[i].r, k) == ring_at(rings[i].r, k))) nan++;
            off += snprintf(rb + off, sizeof rb - (size_t)off,
                            " %s(len=%d max=%.1f nan=%d)", rings[i].n, rings[i].r->len,
                            ring_max(rings[i].r, 0), nan);
        }
        logf_("rx_bps=%.1f tx_bps=%.1f · %s · 签名 %.1fms 绘制 %.1fms 上屏 %.1fms 采样 %.1fms 远端 %.1fms ·%s",
              M.rx_bps, M.tx_bps, buf, dbg_ms_sig, dbg_ms_render, dbg_ms_blit, dbg_ms_samp,
              dbg_ms_rd, rb);
    }
    dbg_ms_sig = dbg_ms_render = dbg_ms_blit = dbg_ms_samp = dbg_ms_rd = 0;
    t_last = t;
    dbg_iter = dbg_ev = dbg_poll_rd = dbg_poll_to = dbg_poll_oth = dbg_redraw = dbg_sample = 0;
    dbg_remote_frames = 0;
    memset(dbg_evtype, 0, sizeof dbg_evtype);
}

/* ---------------- 主循环 ---------------- */
int main(int argc, char **argv) {
    int once = 0, dump = 0;
    /* 先读配置文件，再让命令行覆盖（--remote > 文件；幕参数同理） */
    read_conf_file();
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--once")) once = 1;
        else if (!strcmp(argv[i], "--dump")) dump = 1;
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) SCALE = atof(argv[++i]);
        else if (!strcmp(argv[i], "--panel-idle") && i + 1 < argc) CFG_PANEL_IDLE = atof(argv[++i]);
        else if (!strcmp(argv[i], "--login-back") && i + 1 < argc) CFG_LOGIN_BACK = atof(argv[++i]);
        else if (!strcmp(argv[i], "--input-grace") && i + 1 < argc) CFG_INPUT_GRACE = atof(argv[++i]);
        else if (!strcmp(argv[i], "--sleep-off") && i + 1 < argc) CFG_SLEEP_OFF = atof(argv[++i]);
        else if (!strcmp(argv[i], "--remote") && i + 1 < argc)
            snprintf(CFG_REMOTE, sizeof CFG_REMOTE, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--kiosk")) CFG_KIOSK = 1;
        else if (!strcmp(argv[i], "--remote2") && i + 1 < argc) {
            /* 已废弃：hpcmd 下线后没有第二台机。留着只为「老启动脚本不炸」，只记一行提示。 */
            snprintf(CFG_REMOTE2_DEAD, sizeof CFG_REMOTE2_DEAD, "%s", argv[++i]);
        }
        else if (!strcmp(argv[i], "--scene") && i + 1 < argc)
            snprintf(CFG_SCENE, sizeof CFG_SCENE, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--rotate") && i + 1 < argc) CFG_ROTATE = atof(argv[++i]);
        else if (!strcmp(argv[i], "--transition") && i + 1 < argc) {
            int fx = scene_fx_by_name(argv[++i]);
            if (fx < 0) {
                fprintf(stderr, "未知特效：%s（可用：cut dip push cover）\n", argv[i]);
                return 2;
            }
            CFG_TRANS = fx;
        } else if (!strcmp(argv[i], "--theme") && i + 1 < argc) {
            const char *v = argv[++i];
            if (!strcmp(v, "day")) CFG_THEME = 0;
            else if (!strcmp(v, "night")) CFG_THEME = 1;
            else if (!strcmp(v, "auto")) CFG_THEME = -1;
            else { fprintf(stderr, "未知主题：%s（auto|day|night）\n", v); return 2; }
        } else if (!strcmp(argv[i], "--theme-day") && i + 1 < argc) {
            int m = parse_hhmm(argv[++i]);
            if (m < 0) { fprintf(stderr, "时间格式应为 HH:MM\n"); return 2; }
            CFG_THEME_DAY = m;
        } else if (!strcmp(argv[i], "--theme-night") && i + 1 < argc) {
            int m = parse_hhmm(argv[++i]);
            if (m < 0) { fprintf(stderr, "时间格式应为 HH:MM\n"); return 2; }
            CFG_THEME_NIGHT = m;
        } else if (!strcmp(argv[i], "--list-scenes")) {
            for (int k = 0; k < scenes_count(); k++)
                printf("%s\t%s\n", scene_at(k)->id, scene_at(k)->label);
            return 0;
        }
        else if (!strcmp(argv[i], "--lock") && i + 1 < argc) CFG_LOCK = argv[++i];
        else if (!strcmp(argv[i], "--state") && i + 1 < argc) {
            if (!strcmp(argv[i + 1], "desktop")) FORCE_STATE = ST_DESKTOP;
            else if (!strcmp(argv[i + 1], "panel")) FORCE_STATE = ST_PANEL;
            else if (!strcmp(argv[i + 1], "login")) FORCE_STATE = ST_LOGIN;
            i++;
        }
    }

    if (CFG_TRENDING[0]) scene_trending_file(CFG_TRENDING);
    if (CFG_NEWS[0]) scene_news_file(CFG_NEWS);
    signal(SIGUSR1, on_sig); /* 立即锁屏（进登录页） */
    signal(SIGUSR2, on_sig); /* 立即回面板 */
    signal(SIGPIPE, SIG_IGN);
    DBG = getenv("HPOPI5B_PANEL_DEBUG") != NULL;
    /* 生产服务器优先级：--remote > $HPOPI5B_REMOTE > /etc/hpopi5b-panel.conf */
    if (!CFG_REMOTE[0]) read_conf_file();
    if (!CFG_REMOTE[0] && getenv("HPOPI5B_REMOTE"))
        snprintf(CFG_REMOTE, sizeof CFG_REMOTE, "%s", getenv("HPOPI5B_REMOTE"));
    metrics_init(&M);
    remote_init(&R, CFG_REMOTE);
    FEED.enabled = R.enabled;
    if (CFG_REMOTE2_DEAD[0])
        fprintf(stderr, "hpopi5b-panel: remote2=%s 已废弃（2026-11-06 单机化），已忽略\n",
                CFG_REMOTE2_DEAD);
    FEED.age_s = -1;
    snprintf(FEED.target, sizeof FEED.target, "%s", R.target);
    auth_default_user(g_user, sizeof g_user);

    if (dump) {
        for (int i = 0; i < 4; i++) {
            metrics_sample(&M, i == 3);
            if (i < 3) usleep(1000000);
        }
        printf("host=%s iface=%s ip=%s net_up=%d ts=%s(%d)\n", M.host, M.iface, M.ip, M.net_up,
               M.ts_ip, M.ts_up);
        printf("cpu=%.1f%% ncore=%d load=%.2f %.2f %.2f temp=%.1fC gov=%s %ldMHz\n", M.cpu_pct,
               M.ncore, M.load[0], M.load[1], M.load[2], M.temp_c, M.governor, M.freq_mhz);
        printf("mem=%.0fM/%.0fM (%.0f%%) swap=%.0fM/%.0fM disk=%.0f%%\n", M.mem_used_mb,
               M.mem_total_mb, M.mem_pct, M.swap_used_mb, M.swap_total_mb, M.disk_used_pct);
        printf("net_rate rx=%.1fB/s tx=%.1fB/s\n", M.rx_bps, M.tx_bps);
        printf("nproc=%d top:", M.nproc);
        for (int i = 0; i < M.ntop; i++) printf(" %s(%.0fM)", M.top[i].name, M.top[i].rss_mb);
        printf("\n");

        /* 生产服务器：真的拉一次，证明链路是通的（部署自检靠这个判据） */
        if (R.enabled) {
            double t0 = now_s();
            remote_start(&R, t0);
            int ok = 0;
            while (now_s() - t0 < 12.0 && !ok) {
                struct pollfd p = {remote_fd(&R), POLLIN, 0};
                if (p.fd < 0) break;
                poll(&p, 1, 500);
                ok = remote_read(&R, now_s());
                remote_reap(&R, now_s());
                if (!R.running) break;
            }
            remote_stop(&R);
            if (ok) {
                printf("remote host=%s ncore=%d cpu=%.1f%% mem=%.0fM/%.0fM disk=%.0f%% "
                       "temp=%.1fC dock=%d nproc=%d ip=%s\n",
                       R.m.host, R.m.ncore, R.m.cpu_pct, R.m.mem_used_mb, R.m.mem_total_mb,
                       R.m.disk_used_pct, R.m.temp_c, R.m.ndocker, R.m.nproc, R.m.ip);
                printf("remote_top:");
                for (int i = 0; i < R.m.ntop && i < 5; i++)
                    printf(" %s(%.0fM)", R.m.top[i].name, R.m.top[i].rss_mb);
                printf("\n");
            } else {
                printf("remote FAILED（12 秒内没拿到一帧；手动验：ssh -T %s 'perl -' < "
                       "tools/probe-remote.pl）\n", R.target);
            }
        }
        char so[32] = {0};
        sleep_off_desc(so, sizeof so);
        printf("panel_idle=%.0fs login_back=%.0fs grace=%.1fs sleep_off=%s user=%s remote=%s\n",
               CFG_PANEL_IDLE, CFG_LOGIN_BACK, CFG_INPUT_GRACE, so, g_user,
               CFG_REMOTE[0] ? CFG_REMOTE : "(未配置)");
        return 0;
    }

    /* 单实例锁：两个 hpopi5b-panel 会互相抢抬窗（都 override-redirect），
       屏幕会变成两张界面互相覆盖 —— 实测踩到（手工起一个 + 守护循环又起一个）。
       ⚠️ 必须放在 `--dump` 之后：dump 不需要 X、也不碰窗口，却会被正在跑的实例挡住
       （实测：deploy 里的 `--dump` 自检返回 3、打不出参数，看着像「默认值被改回去了」）。 */
    int lockfd = -1;
    if (!dump && !once) {          /* --dump / --once 不建窗口，不需要抢锁 */
        lockfd = open(CFG_LOCK, O_CREAT | O_RDWR, 0644);
        if (lockfd >= 0 && flock(lockfd, LOCK_EX | LOCK_NB) != 0) {
            fprintf(stderr, "hpopi5b-panel: 已有实例在运行（%s 被锁）⇒ 退出\n", CFG_LOCK);
            return 3;
        }
    }

    /* 主题：TP 必须在第一次 v_bg 之前就位（surface_init → bg_init 会烤渐变） */
    theme_night = CFG_THEME >= 0 ? CFG_THEME
                                 : theme_night_at(time(NULL), CFG_THEME_DAY, CFG_THEME_NIGHT);
    theme_set(theme_night);
    theme_check_at = now_s() + 10.0;

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "hpopi5b-panel: 无法打开 X display（DISPLAY=%s）\n",
                getenv("DISPLAY") ? getenv("DISPLAY") : "(null)");
        return 1;
    }
    scr = DefaultScreen(dpy);
    SW = DisplayWidth(dpy, scr);
    SH = DisplayHeight(dpy, scr);
    root = RootWindow(dpy, scr);

    /* 屏幕尺寸变化（换接口）时重建窗口 */
    XSetWindowAttributes attrs;
    attrs.override_redirect = True;   /* 自己管位置：面板要盖住整个屏幕（含 polybar） */
    attrs.background_pixel = BlackPixel(dpy, scr);
    win = XCreateWindow(dpy, root, 0, 0, (unsigned)SW, (unsigned)SH, 0,
                        CopyFromParent, InputOutput, CopyFromParent,
                        CWOverrideRedirect | CWBackPixel, &attrs);
    XStoreName(dpy, win, "hpopi5b-panel");
    XSelectInput(dpy, win,
                 ExposureMask | KeyPressMask | ButtonPressMask | PointerMotionMask |
                     StructureNotifyMask | VisibilityChangeMask);
    /* 窗口先不映射：映射/抓键盘/设焦点统一由 win_show() 做
       （对未映射窗口调 SetInputFocus 会 BadMatch 直接退出，实测 Major opcode 42） */

    /* ⚠️ GC 必须关掉 graphics_exposures（默认 True）：
       否则每次 XCopyArea 上屏都会回一个 NoExpose 事件 ⇒ poll 立刻返回
       ⇒ 主循环自激空转（实测 35 it/s、CPU 3.3%，面板上的 CPU% 变成 KTV 乱跳）。 */
    XGCValues gcv;
    gcv.graphics_exposures = False;
    blitgc = XCreateGC(dpy, win, GCGraphicsExposures, &gcv);
    surface_init();   /* 建离屏后备 pixmap + cairo 上下文（不直接画窗口） */

    /* 初始状态 */
    state = CFG_KIOSK ? ST_PANEL : ((FORCE_STATE >= 0) ? FORCE_STATE : ST_DESKTOP);
    state_enter = now_s();
    /* 幕：--scene 钉住一幕；否则从 panel 开始。kiosk 默认 30s 轮换，桌面/锁屏默认不轮换 */
    {
        double rot = CFG_ROTATE;
        if (rot < 0) rot = CFG_KIOSK ? 30.0 : 0.0;
        int pin = CFG_SCENE[0] ? scene_index(CFG_SCENE) : -1;
        if (CFG_SCENE[0] && pin < 0)
            logf_("未知幕「%s」⇒ 退回 panel（用 --list-scenes 看可用幕）", CFG_SCENE);
        scenes_reset(state_enter, pin, rot, CFG_TRANS);
    }
    char so[32] = {0};
    sleep_off_desc(so, sizeof so);
    logf_("启动：初始 %s · panel_idle=%.0fs · login_back=%.0fs · grace=%.1fs · "
          "sleep_off=%s · scale=%.2f · %dx%d",
          state_name(state), CFG_PANEL_IDLE, CFG_LOGIN_BACK, CFG_INPUT_GRACE, so,
          SCALE, SW, SH);
    logf_("主题：%s · %s（白天 %02d:%02d · 夜间 %02d:%02d）", theme_night ? "夜间" : "白天",
          CFG_THEME >= 0 ? "钉住" : "自动切换", CFG_THEME_DAY / 60, CFG_THEME_DAY % 60,
          CFG_THEME_NIGHT / 60, CFG_THEME_NIGHT % 60);
    if (state != ST_DESKTOP) win_show();

    /* ⭐ 启动自愈：本程序的 screen_off 只反映「自己发过什么」，不反映真实 DPMS 状态。
       旧版 sleep_off=3600 关过屏、或者在「永不关屏」上线前就停在 DPMS Off，
       新实例会以为屏是亮的 ⇒ 唤醒路径永不触发 ⇒ 屏幕永远黑着（实测：部署后是 DPMS Off）。
       「永不关屏」模式下启动就无条件开一次屏，把它掀回来。 */
    if (CFG_SLEEP_OFF <= 0) {
        dpms(1);
        logf_("永不关屏模式：启动时确保屏幕开启（防止上次遗留的 DPMS Off）");
    }

    /* 预填一点曲线数据 */
    for (int i = 0; i < 3; i++) {
        metrics_sample(&M, 0);
        usleep(120000);
    }
    /* ⚠️ 不要「用当前值把曲线预填满」：那个值来自启动瞬间的采样窗口
       （实测常是 0，偶尔是一个 120ms 的突发尖峰），于是曲线要么贴底、要么被假尖峰
       把量程顶上去把真数据压扁 —— 等于伪造 5 分钟历史。改成诚实生长 + 零基线。
       背景见 pitfalls/2026-10-18-*（本轮实测：rx/tx 的 [0]=0.0 而 [299]=131）。 */

    int tick = 0;
    for (;;) {
        /* --- 事件 --- */
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            if (DBG) { dbg_ev++; if (ev.type < 64) dbg_evtype[ev.type]++; }
            switch (ev.type) {
            case KeyPress: on_key(&ev.xkey); break;
            case MotionNotify: on_motion(); break;
            case ButtonPress: on_button(); break;
            case Expose:
                if (state != ST_DESKTOP) redraw_force();
                break;
            case ConfigureNotify:
                if (ev.xconfigure.width != SW || ev.xconfigure.height != SH) {
                    SW = ev.xconfigure.width;
                    SH = ev.xconfigure.height;
                    surface_init();   /* 尺寸变了 ⇒ 重建后备缓冲 */
                    if (state != ST_DESKTOP) redraw_force();
                }
                break;
            default: break;
            }
        }

        /* --- 信号触发的状态跳转 --- */
        if (g_sig_lock) {
            g_sig_lock = 0;
            if (state == ST_DESKTOP) set_state(ST_LOGIN);
            else if (state == ST_PANEL) set_state(ST_LOGIN);
        }
        if (g_sig_panel) {
            g_sig_panel = 0;
            set_state(ST_PANEL);
        }

        /* --- 采样：固定 0.5s 节流 ---
           ⚠️ 不要「每轮循环采样一次」：循环速率一旦被事件带快，
           采样窗口就变成几十毫秒 ⇒ CPU% 会跳成 KTV（实测）。 */
        static double t_sample;
        static int nsamp;
        if (now_s() - t_sample >= 1.0) {
            t_sample = now_s();
            dbg_sample++;
            /* 进程榜每 3s 一次（它最贵：扫 /proc ≈ 30ms）。
               ⚠️ 但**屏幕上画的是远端那台**时，本地榜一个像素都用不到 ⇒ 直接不扫。
               kiosk 常态就是「远端在线 + 停在面板」；实测省下的 CPU 在噪声里（<0.2pp），
               但「不采集用不到的东西」本身是对的，留着。 */
            int want_procs = (nsamp % 3) == 0 && !(R.enabled && state == ST_PANEL);
            double ts0 = now_s();
            metrics_sample(&M, want_procs);
            dbg_ms_samp += (now_s() - ts0) * 1000;
            nsamp++;
        }

        /* --- 生产服务器采集 ---
           只在「有人在看」的时候跑：面板显示 → 拉起；回桌面/登录页 → 停掉（省流量、省远端 CPU）。
           ⚠️ 但**保留已采集的数据**：回面板时立刻有完整一帧可画，不会白掉几秒。 */
        if (R.enabled) {
            double tw = now_s();
            if (state == ST_PANEL) {
                remote_start(&R, tw);
            } else if (R.running) {
                remote_stop(&R);
            }
            remote_reap(&R, tw);
            FEED.online = remote_online(&R, tw);
            FEED.age_s = remote_age(&R, tw);
            if (FEED.age_s < 0 && R.running) FEED.age_s = -1;
        }

        /* --- 状态机 --- */
        double idle = x_idle();
        double in_state = now_s() - state_enter;

        if (state == ST_DESKTOP) {
            if (idle >= CFG_PANEL_IDLE) set_state(ST_PANEL);
        } else if (state == ST_PANEL) {
            /* CFG_SLEEP_OFF ≤ 0 = 永不关屏（绝不能写成裸 idle>=CFG_SLEEP_OFF：
               那样 0 会变成「立刻关屏」，语义正好反过来） */
            if (CFG_SLEEP_OFF > 0 && idle >= CFG_SLEEP_OFF && !screen_off) {
                logf_("面板空闲 %.0fs ⇒ 关屏（DPMS）", idle);
                dpms(0);
            }
            /* 从面板「被叫醒」的情况在上面的输入事件里处理；
               这里兜底：如果 idle 突然变小（输入事件被我们漏掉），也切登录页 */
            if (idle < 0.5 && in_state > CFG_INPUT_GRACE) set_state(ST_LOGIN);
        } else if (state == ST_LOGIN) {
            if (in_state > 1.5 && idle >= CFG_LOGIN_BACK) {
                logf_("登录页空闲 %.0fs ⇒ 回面板", idle);
                set_state(ST_PANEL);
            }
        }

        /* --- 幕轮换 / 转场推进（返回 1 = 需要一帧，签名里已含进度） --- */
        if (state == ST_PANEL) (void)scenes_tick(now_s());

        /* --- 主题：固定时刻切换，先淡出到黑、换色、再淡入 ---
           每 10s 看一眼钟（自动模式下）；淡变全长 2×0.45s，期间每帧重画。 */
        if (CFG_THEME < 0) {
            double nw = now_s();
            if (!theme_fade && nw >= theme_check_at) {
                theme_check_at = nw + 10.0;
                int want = theme_night_at(time(NULL), CFG_THEME_DAY, CFG_THEME_NIGHT);
                if (want != theme_night) {
                    if (state == ST_DESKTOP) {
                        theme_apply(want);          /* 没人看，直接换 */
                    } else {
                        theme_fade = 1;
                        theme_fade_t0 = nw;
                        theme_fade_pending = want;
                        theme_fade_swapped = 0;
                    }
                }
            }
            if (theme_fade) {
                double t = now_s() - theme_fade_t0;
                if (!theme_fade_swapped && t >= THEME_FADE) {
                    theme_fade_swapped = 1;
                    theme_apply(theme_fade_pending);  /* 全黑那一刻换色 + 重画 bgpix */
                }
                if (t >= THEME_FADE * 2.0) {
                    theme_fade = 0;
                    theme_fade_swapped = 0;
                    g_sig_valid = 0;
                }
            }
        }

        /* --- 重绘（面板 1Hz；登录页只闪光标） --- */
        if (theme_fade) redraw_force();
        else if (state == ST_PANEL) {
            redraw();
        } else if (state == ST_LOGIN) {
            int c = ((int)(now_s() * 2)) % 2;
            if (c != cursor_on) {
                cursor_on = c;
                redraw();
            } else if ((tick % 10) == 0) {
                redraw(); /* 时钟/等待秒数 */
            }
        }

        /* --- 抬窗：polybar 等 override-redirect 窗口比面板晚映射时会压在上面（实测） --- */
        if (state != ST_DESKTOP && (tick % 2) == 0) win_raise();

        if (once) {
            redraw();
            break;
        }
        tick++;
        dbg_iter++;
        dbg_tick();
        /* poll 两个源：X 连接 + 远程机的 ssh 管道（非阻塞，绝不引入第二个线程） */
        struct pollfd p[2];
        p[0].fd = ConnectionNumber(dpy);
        p[0].events = POLLIN;
        p[0].revents = 0;
        p[1].fd = remote_fd(&R);
        p[1].events = POLLIN;
        p[1].revents = 0;
        int nfd = 1;
        if (p[1].fd >= 0) nfd = 2;
        int timeout = state == ST_DESKTOP ? 1000 : 500;
        /* 转场要 30ms 一帧（~33fps，0.55s 共 ~18 帧）；平时仍 500ms 一次 */
        if (scenes_transitioning()) timeout = 30;
        if (theme_fade) timeout = 25;   /* 淡变 ~40fps：够顺，又不至于把 CPU 顶起来 */
        int pr = poll(p, (unsigned)nfd, timeout);
        if (DBG) {
            if (pr == 0) dbg_poll_to++;
            else if (p[0].revents & POLLIN) dbg_poll_rd++;
            else dbg_poll_oth++;
        }
        if (nfd > 1 && (p[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            if (remote_read(&R, now_s())) dbg_remote_frames++;
        }
    }

    remote_stop(&R);
    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    if (back) XFreePixmap(dpy, back);
    XCloseDisplay(dpy);
    return 0;
}
