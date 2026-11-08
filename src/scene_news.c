/* scene_news.c —— 幕「新闻」：左「国内热点」/ 右「科技」
 *
 * 用户口径（2026-11-08）：「想加一幕，关于新闻的」「空间还挺多」「两列吧」。
 *
 * 数据通路与 trending 幕同一个原则：**面板自己从不联网**。
 *   tools/fetch-news.py（头条热榜 JSON + Solidot/少数派/36氪 RSS）
 *        └─ 原子写 /var/cache/hpopi5b-panel/news.tsv   ← systemd timer 每 30 分钟
 *   scene_news.c  stat() 比对 mtime/size，文件变了才重读；渲染永远不被网络阻塞。
 *
 * 诚实原则：抓不到就继续显示上一份，右上角写「N 分钟前同步」，超过 90 分钟转黄。
 * **不灌假数据**、不预填、不因为「看起来空」编条目（AGENTS.md 第 5/7 节）。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "theme.h"
#include "scene.h"

#define NW_MAX 48

typedef struct {
    char title[192];
    char src[24];   /* 来源短名（头条 / Solidot / 少数派 …） */
    char meta[24];  /* 热点：热度的人话写法（1315万）；科技：空 */
    long ts;        /* 科技：unix 时间戳；热点：0 */
} nw_row_t;

static nw_row_t hot[NW_MAX], tech[NW_MAX];
static int nhot, ntech;
static char cache_path[256] = "/var/cache/hpopi5b-panel/news.tsv";
static struct stat lst;
static int loaded;

static PangoFontDescription *FD_HEAD, *FD_ROW, *FD_META, *FD_RANK;
static double H_ROW;

static void init_fonts(void) {
    if (FD_ROW) return;
    FD_HEAD = v_fd_label();  /* 列标题：10pt，别抢过正文 */
    FD_ROW = v_fd_desc();    /* 新闻标题：13pt（和 trending 描述同号，两个幕观感一致） */
    FD_META = v_fd_small();  /* 热度 / 来源 · 时间 */
    FD_RANK = v_fd_small();
    H_ROW = v_text_h(FD_ROW, "Ag");
}

void scene_news_file(const char *p) {
    if (!p || !*p) return;
    snprintf(cache_path, sizeof cache_path, "%s", p);
}

/* 按真实像素宽度截断（CJK 不能按字符数切：会溢出，见 trending 里同名函数） */
static void fit(char *out, size_t outn, PangoFontDescription *fd, const char *s, double maxw) {
    if (maxw <= 0) {
        out[0] = 0;
        return;
    }
    if (v_text_w(fd, s) <= maxw) {
        snprintf(out, outn, "%s", s);
        return;
    }
    int n = (int)strlen(s);
    int lo = 0, hi = n;
    double budget = maxw - v_text_w(fd, "…");
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        while (mid > lo && ((unsigned char)s[mid] & 0xC0) == 0x80) mid--;
        if (mid == lo) break;
        char tmp[512];
        snprintf(tmp, sizeof tmp, "%.*s", mid, s);
        if (v_text_w(fd, tmp) <= budget) lo = mid; else hi = mid - 1;
    }
    while (lo > 0 && ((unsigned char)s[lo] & 0xC0) == 0x80) lo--;
    snprintf(out, outn, "%.*s…", lo, s);
}

static void fmt_age(long ts, long now, char *out, size_t n) {
    if (ts <= 0) {
        out[0] = 0;
        return;
    }
    long d = now - ts;
    if (d < 0) d = 0;
    if (d > 999L * 86400) d = 999L * 86400; /* 封顶：%ldd 最大 5 字节，别让编译器担心截断 */
    if (d < 90) snprintf(out, n, "刚刚");
    else if (d < 3600) snprintf(out, n, "%ldm", d / 60);
    else if (d < 86400) snprintf(out, n, "%ldh", d / 3600);
    else snprintf(out, n, "%ldd", d / 86400);
}

/* 一行分成 4 列（kind / title / src / meta-or-ts）。
 * ⚠️ 先去掉行尾 \r\n 再切列：切列会把 \t 写成 NUL，strcspn 之后就停在第一个 NUL 上，
 *    行尾换行永远去不掉 ⇒ 标题里带 \n ⇒ pango 认为它是两行（这个坑 2026-11-06 踩过）。 */
static void load(void) {
    struct stat st;
    if (stat(cache_path, &st) != 0) {
        nhot = ntech = 0;
        loaded = 1;
        return;
    }
    if (loaded && st.st_mtime == lst.st_mtime && st.st_size == lst.st_size) return;
    lst = st;
    loaded = 1;
    nhot = ntech = 0;

    FILE *f = fopen(cache_path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        line[strcspn(line, "\r\n")] = 0;
        char *p = line;
        char *col[4];
        int nc = 0;
        col[nc++] = p;
        for (; *p && nc < 4; p++)
            if (*p == '\t') {
                *p = 0;
                col[nc++] = p + 1;
            }
        if (nc < 4 || !col[1][0]) continue;

        if (col[0][0] == 'H' && nhot < NW_MAX) {
            nw_row_t *r = &hot[nhot++];
            snprintf(r->title, sizeof r->title, "%s", col[1]);
            snprintf(r->src, sizeof r->src, "%s", col[2]);
            snprintf(r->meta, sizeof r->meta, "%s", col[3]);
        } else if (col[0][0] == 'T' && ntech < NW_MAX) {
            nw_row_t *r = &tech[ntech++];
            snprintf(r->title, sizeof r->title, "%s", col[1]);
            snprintf(r->src, sizeof r->src, "%s", col[2]);
            r->ts = atol(col[3]);
        }
    }
    fclose(f);
}

/* 排版计划缓存：键 = 缓存文件 mtime/size + 列宽 + 分钟。
 * 相对时间「12m」每分钟变一次 ⇒ 分钟也必须进键，否则秒变分钟了还画旧字。 */
static struct {
    time_t mtime;
    off_t size;
    double cw;
    long minute;
    int nh, nt;
} pk = {0, 0, 0, 0, -1, -1};
static char p_hot_title[NW_MAX][256], p_hot_right[NW_MAX][32];
static char p_tech_title[NW_MAX][256], p_tech_right[NW_MAX][48];

static void plan(double cw) {
    long minute = (long)(time(NULL) / 60);
    if (pk.mtime == lst.st_mtime && pk.size == lst.st_size && pk.cw == cw &&
        pk.minute == minute && pk.nh == nhot && pk.nt == ntech)
        return;
    pk.mtime = lst.st_mtime;
    pk.size = lst.st_size;
    pk.cw = cw;
    pk.minute = minute;
    pk.nh = nhot;
    pk.nt = ntech;

    const double textw = cw - L_NW_RANK_W - L_NW_RIGHT_W;
    long now = (long)time(NULL);
    for (int i = 0; i < nhot; i++) {
        snprintf(p_hot_right[i], sizeof p_hot_right[i], "%s", hot[i].meta);
        fit(p_hot_title[i], sizeof p_hot_title[i], FD_ROW, hot[i].title, textw);
    }
    for (int i = 0; i < ntech; i++) {
        char age[16];
        fmt_age(tech[i].ts, now, age, sizeof age);
        if (age[0])
            snprintf(p_tech_right[i], sizeof p_tech_right[i], "%s · %s", tech[i].src, age);
        else snprintf(p_tech_right[i], sizeof p_tech_right[i], "%s", tech[i].src);
        fit(p_tech_title[i], sizeof p_tech_title[i], FD_ROW, tech[i].title, textw);
    }
}

static void draw_col(cairo_t *cr, double x, double cw, double y0, double avail, int kind) {
    int n = kind == 'H' ? nhot : ntech;
    if (n <= 0) {
        v_text(cr, x + L_NW_RANK_W, y0 + 8, FD_META, C_OVERLAY0, "还没取到（看抓取日志）");
        return;
    }
    int rows = (int)(avail / L_NW_ROW_H);
    if (rows < 1) rows = 1;
    if (rows > n) rows = n;

    /* 零头均摊到行距里（封顶）：两列都收在接近屏底的位置，不留说不清的空白带 */
    double pad = (avail - rows * L_NW_ROW_H) / rows;
    if (pad < 0) pad = 0;
    if (pad > L_NW_ROW_PAD_MAX) pad = L_NW_ROW_PAD_MAX;

    for (int i = 0; i < rows; i++) {
        double y = y0 + i * (L_NW_ROW_H + pad);
        v_text(cr, x, y + 6, FD_RANK, C_OVERLAY0, "%02d", i + 1);
        if (kind == 'H') {
            v_text(cr, x + L_NW_RANK_W, y + 5, FD_ROW, C_TEXT, "%s", p_hot_title[i]);
            v_text_right(cr, x + cw, y + 7, FD_META, C_YELLOW, "%s", p_hot_right[i]);
        } else {
            v_text(cr, x + L_NW_RANK_W, y + 5, FD_ROW, C_TEXT, "%s", p_tech_title[i]);
            v_text_right(cr, x + cw, y + 7, FD_META, C_OVERLAY1, "%s", p_tech_right[i]);
        }
        if (i + 1 < rows) v_rule(cr, x, y + L_NW_ROW_H + pad - 5, cw);
    }
}

static void draw(cairo_t *cr, const view_ctx_t *v, const scene_env_t *e) {
    init_fonts();
    load();
    (void)e;

    const double mx = L_MARGIN, mw = v->lw - mx * 2;
    const double gap = L_COL_GAP, cw = (mw - gap) / 2;
    const double y0 = L_NW_Y0;

    v_text(cr, mx, L_TOP_Y + 13, FD_HEAD, C_MAUVE, "新闻");

    long age = (long)(time(NULL) - lst.st_mtime);
    char head[64];
    uint32_t headcol = C_OVERLAY0;
    if (!loaded || lst.st_mtime == 0) snprintf(head, sizeof head, "还没同步过");
    else if (age < 90) snprintf(head, sizeof head, "刚刚同步");
    else if (age < 3600) snprintf(head, sizeof head, "%ld 分钟前同步", age / 60);
    else snprintf(head, sizeof head, "%ld 小时前同步", age / 3600);
    if (lst.st_mtime != 0 && age > 5400) headcol = C_YELLOW; /* 3 个抓取周期：该看一眼日志了 */
    v_text_right(cr, v->lw - mx, L_TOP_Y + 13, FD_HEAD, headcol, "%s · 热点 %d · 科技 %d", head,
                 nhot, ntech);
    v_rule(cr, mx, L_RULE1_Y, mw);

    /* 列标题 + 一条细线（分区只靠细线，不做卡片） */
    v_text(cr, mx, y0, FD_HEAD, C_SUBTEXT1, "国内热点");
    v_text(cr, mx + cw + gap, y0, FD_HEAD, C_SUBTEXT1, "科技");
    v_rule(cr, mx, y0 + L_NW_HEAD_H - 8, cw);
    v_rule(cr, mx + cw + gap, y0 + L_NW_HEAD_H - 8, cw);

    const double top = y0 + L_NW_HEAD_H;
    /* 26 = 页脚高度（与面板同口径；可用高度从 lh 现算，换屏/换 scale 都不会压页脚） */
    double avail = v->lh - 26 - top;
    if (avail < 0) avail = 0;

    if (nhot == 0 && ntech == 0) {
        v_text_center(cr, v->lw / 2, v->lh / 2 - 14, v_fd_big(), C_OVERLAY0, "新闻还没同步");
        v_text_center(cr, v->lw / 2, v->lh / 2 + 20, v_fd_label(), C_OVERLAY0,
                      "systemctl start hpopi5b-news.service");
        v_text_center(cr, v->lw / 2, v->lh / 2 + 42, v_fd_small(), C_OVERLAY0, "%s", cache_path);
        return;
    }

    plan(cw);
    draw_col(cr, mx, cw, top, avail, 'H');
    draw_col(cr, mx + cw + gap, cw, top, avail, 'T');

    if (getenv("LAYOUT_DEBUG"))
        fprintf(stderr,
                "news: hot=%d tech=%d cw=%.1f y0=%.1f avail=%.1f rows=%d H_ROW=%.1f lh=%.1f\n",
                nhot, ntech, cw, top, avail, (int)(avail / L_NW_ROW_H), H_ROW, v->lh);
}

/* 缓存文件变了 / 每分钟一次的相对时间要重画 */
static unsigned long long sig(const scene_env_t *e) {
    load();
    unsigned long long h = 1469598103934665603ULL;
    h = (h ^ (unsigned long long)lst.st_mtime) * 1099511628211ULL;
    h = (h ^ (unsigned long long)(nhot * 64 + ntech)) * 1099511628211ULL;
    h = (h ^ (unsigned long long)((long)e->wall / 60)) * 1099511628211ULL;
    return h;
}

const scene_t SCENE_NEWS = {"news", "新闻（国内热点 + 科技）", draw, sig};