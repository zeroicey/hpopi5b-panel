/* scene_news.c —— 幕「新闻」：左「国内热点」/ 右「科技」两列
 *
 * 用户口径（2026-11-08）：「想加一幕，关于新闻的」「空间还挺多」。
 * 用户口径（2026-11-09）：「效果一般般很单调 空间利用率不够」「国内热点和科技这一行标题可以删掉」。
 *   ⇒ 第二版（本轮）：**三列**（热点 1 列 + 科技 2 列）、删列标题行、热点列首条放大、
 *     每行带第二条信息线（热度条 / 来源·时间），两列科技之间用竖细线分区。
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
    double heat;    /* 热点：由 meta 解析出的数值，只用于画相对条（排序由抓取端定） */
    long ts;        /* 科技：unix 时间戳；热点：0 */
} nw_row_t;

static nw_row_t hot[NW_MAX], tech[NW_MAX];
static int nhot, ntech;
static char cache_path[256] = "/var/cache/hpopi5b-panel/news.tsv";
static struct stat lst;
static int loaded;

static PangoFontDescription *FD_HEAD, *FD_ROW, *FD_LEAD, *FD_META, *FD_RANK;
static double H_ROW;

static void init_fonts(void) {
    if (FD_ROW) return;
    FD_HEAD = v_fd_label();  /* 顶栏统计：10pt，别抢过正文 */
    FD_ROW = v_fd_desc();    /* 新闻标题：13pt（和 trending 描述同号，两个幕观感一致） */
    FD_LEAD = v_fd_value();  /* 热点首条：17pt，整屏唯一的视觉重心 */
    FD_META = v_fd_small();  /* 热度 / 来源 / 时间：9pt */
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
        while (mid > lo && ((unsigned char)s[mid] & 0xC0) == 0x80) mid--; /* 退到 UTF-8 首字节 */
        /* ⚠️ 退到 lo 时必须 break：否则 mid==lo 且「够窄」⇒ lo 不变 ⇒ 死循环
         *    （2026-11-09 把这段重写丢了这一行，面板卡在 win_show→render 里不出窗口） */
        if (mid == lo) break;
        char probe[512];
        snprintf(probe, sizeof probe, "%.*s", mid, s);
        if (v_text_w(fd, probe) <= budget) lo = mid;
        else hi = mid - 1;
    }
    while (lo > 0 && ((unsigned char)s[lo] & 0xC0) == 0x80) lo--;
    snprintf(out, outn, "%.*s…", lo, s);
}

/* unix 时间戳 → 「刚刚 / 12m / 21h / 3d」
 * ⚠️ 天数封顶 999：`snprintf("%ldd")` 装进 char[16] 时，未封顶会触发 -Wformat-truncation。 */
static void fmt_age(long ts, long now, char *out, size_t n) {
    long d = now - ts;
    if (ts <= 0 || d < 0) {
        out[0] = 0;
        return;
    }
    if (d < 90) snprintf(out, n, "刚刚");
    else if (d < 3600) snprintf(out, n, "%ldm", d / 60);
    else if (d < 86400) snprintf(out, n, "%ldh", d / 3600);
    else {
        if (d > 999L * 86400) d = 999L * 86400;
        snprintf(out, n, "%ldd", d / 86400);
    }
}

/* 「1428万」→ 1428e4；只用来算相对条长，所以精度无所谓 */
static double heat_of(const char *s) {
    if (!s || !*s) return 0;
    char *end = NULL;
    double v = strtod(s, &end);
    if (end == s) return 0;
    if (strstr(s, "亿")) v *= 1e8;
    else if (strstr(s, "万")) v *= 1e4;
    return v;
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
            r->heat = heat_of(col[3]);
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
    double hotcw, techcw;
    long minute;
    int nh, nt;
} pk = {0, 0, 0, 0, 0, -1, -1};
static char p_lead_title[192];
static char p_hot_title[NW_MAX][256], p_hot_right[NW_MAX][32];
static char p_tech_title[NW_MAX][256], p_tech_src[NW_MAX][24], p_tech_age[NW_MAX][16];
static double p_hot_frac[NW_MAX], p_hot_numw[NW_MAX];
static double p_lead_numw;

static void plan(double hotcw, double techcw) {
    long minute = (long)(time(NULL) / 60);
    if (pk.mtime == lst.st_mtime && pk.size == lst.st_size && pk.hotcw == hotcw &&
        pk.techcw == techcw && pk.minute == minute && pk.nh == nhot && pk.nt == ntech)
        return;
    pk.mtime = lst.st_mtime;
    pk.size = lst.st_size;
    pk.hotcw = hotcw;
    pk.techcw = techcw;
    pk.minute = minute;
    pk.nh = nhot;
    pk.nt = ntech;

    long now = (long)time(NULL);
    /* 相对长度以榜首为 1.0：热榜本身就是按热度降序给的 */
    double maxheat = nhot > 0 ? hot[0].heat : 0;

    snprintf(p_lead_title, sizeof p_lead_title, "%s",
             nhot > 0 ? hot[0].title : ""); /* 首条走折行，不预先截断 */
    p_lead_numw = (nhot > 0 && hot[0].meta[0]) ? v_text_w(FD_ROW, hot[0].meta) : 0;
    for (int i = 0; i < nhot; i++) {
        double tw = hotcw - L_NW_RANK_W;
        snprintf(p_hot_right[i], sizeof p_hot_right[i], "%s", hot[i].meta);
        p_hot_numw[i] = hot[i].meta[0] ? v_text_w(FD_META, hot[i].meta) : 0;
        p_hot_frac[i] = (maxheat > 0 && hot[i].heat > 0) ? hot[i].heat / maxheat : -1;
        if (p_hot_frac[i] > 1) p_hot_frac[i] = 1;
        fit(p_hot_title[i], sizeof p_hot_title[i], FD_ROW, hot[i].title, tw);
    }
    for (int i = 0; i < ntech; i++) {
        double tw = techcw - L_NW_RANK_W;
        fmt_age(tech[i].ts, now, p_tech_age[i], sizeof p_tech_age[i]);
        snprintf(p_tech_src[i], sizeof p_tech_src[i], "%s", tech[i].src);
        fit(p_tech_title[i], sizeof p_tech_title[i], FD_ROW, tech[i].title, tw);
    }
}

/* 热度条：极淡的轨道 + 暖色游标。轨道让「相对长度」有刻度感，又不会变成卡片底色。 */
static void heat_bar(cairo_t *cr, double x, double y, double w, double frac) {
    if (frac < 0 || w <= 2) return;
    v_set_rgb(cr, C_SURFACE1, 0.45);
    cairo_rectangle(cr, x, y, w, L_NW_BAR_H);
    cairo_fill(cr);
    double fw = w * frac;
    if (fw < 2) fw = 2;
    v_set_rgb(cr, C_YELLOW, 0.80);
    cairo_rectangle(cr, x, y, fw, L_NW_BAR_H);
    cairo_fill(cr);
}

/* 竖细线：三列之间的分区。设计系统里「分区只靠 1px 细线」，竖线同样适用。 */
static void col_rule(cairo_t *cr, double x, double y, double h) {
    v_set_rgb(cr, C_SURFACE2, 0.90);
    cairo_rectangle(cr, x, y, 1.0, h);
    cairo_fill(cr);
}

/* 刻度条 + 数值：**条子左端钉在序号列右缘**（所有条子共用同一条基线 ⇒ 长度可比），
 * 数值贴列右缘（设计系统：量化值一律右对齐）。条长封顶，否则整列变成下划线地毯。 */
static void metric(cairo_t *cr, double x, double xr, double y, PangoFontDescription *fd,
                   double barw, double frac, const char *num, double dy) {
    if (num && num[0]) v_text_right(cr, xr, y, fd, C_YELLOW, "%s", num);
    heat_bar(cr, x, y + dy, barw, frac);
}

static double row_pad(double avail, double used, int rows) {
    if (rows <= 0) return 0;
    double pad = (avail - used) / rows;
    if (pad < 0) pad = 0;
    if (pad > L_NW_COL_PAD_MAX) pad = L_NW_COL_PAD_MAX;
    return pad;
}

/* 热点列：首条放大（17pt，最多两行）+ 其余行「标题 / 热度条 + 热度值」两行式 */
static void draw_hot(cairo_t *cr, double x, double cw, double y0, double avail) {
    if (nhot <= 0) return;
    int rows = (int)((avail - L_NW_LEAD_H) / L_NW_ROW_H);
    if (rows < 0) rows = 0;
    if (rows > nhot - 1) rows = nhot - 1;

    /* ---- 首条：整屏唯一的放大块 ---- */
    double y = y0;
    v_text(cr, x, y + 6, FD_RANK, C_OVERLAY0, "01");
    double th = v_text_wrap(cr, x + L_NW_RANK_W, y + 2, FD_LEAD, C_TEXT, cw - L_NW_RANK_W, 2,
                            "%s", p_lead_title);
    double numw = p_lead_numw;
    double bw = L_NW_BAR_W_LEAD;
    double cap = cw - L_NW_RANK_W - numw - L_NW_BAR_GAP;
    if (bw > cap) bw = cap;
    /* 度量组：一行标题时钉在 y+56，两行时跟随标题底部（块内 92 高，底部留白） */
    double gy = y + 56;
    if (y + 2 + th + 8 > gy) gy = y + 2 + th + 8;
    metric(cr, x + L_NW_RANK_W, x + cw, gy, FD_ROW, bw, p_hot_frac[0], p_hot_right[0], 8);

    /* ---- 其余行：13pt 标题 + 下面一条相对热度条与数值 ---- */
    double used = L_NW_LEAD_H + rows * L_NW_ROW_H;
    double pad = row_pad(avail, used, rows);
    double yy = y0 + L_NW_LEAD_H;
    for (int i = 0; i < rows; i++) {
        double ry = yy + i * (L_NW_ROW_H + pad);
        int k = i + 1;
        v_text(cr, x, ry + 4, FD_RANK, C_OVERLAY0, "%02d", k + 1);
        v_text(cr, x + L_NW_RANK_W, ry + 2, FD_ROW, C_TEXT, "%s", p_hot_title[k]);
        double nw = p_hot_numw[k];
        double w = L_NW_BAR_W;
        double cap = cw - L_NW_RANK_W - nw - L_NW_BAR_GAP;
        if (w > cap) w = cap;
        metric(cr, x + L_NW_RANK_W, x + cw, ry + 26, FD_META, w, p_hot_frac[k], p_hot_right[k], 5);
        // 行间不画细线：两行式的第二行（刻度条 / 来源）本身就是分隔，再加线会变成双线噪声
    }
}

/* 科技列：标题 / 来源 · 时间 两行式。序号在幕内连续（第二列接着第一列数）。 */
static void draw_tech(cairo_t *cr, double x, double cw, double y0, double avail, int from,
                      int count) {
    if (count <= 0) return;
    int rows = (int)(avail / L_NW_ROW_H);
    if (rows < 1) rows = 1;
    if (rows > count) rows = count;

    double used = rows * L_NW_ROW_H;
    double pad = row_pad(avail, used, rows);
    for (int i = 0; i < rows; i++) {
        double ry = y0 + i * (L_NW_ROW_H + pad);
        int k = from + i;
        v_text(cr, x, ry + 4, FD_RANK, C_OVERLAY0, "%02d", k + 1);
        v_text(cr, x + L_NW_RANK_W, ry + 2, FD_ROW, C_TEXT, "%s", p_tech_title[k]);
        v_text(cr, x + L_NW_RANK_W, ry + 26, FD_META, C_OVERLAY0, "%s", p_tech_src[k]);
        if (p_tech_age[k][0])
            v_text_right(cr, x + cw, ry + 26, FD_META, C_OVERLAY1, "%s", p_tech_age[k]);
        /* 科技列保留行间细线（第二行是暗色小字，不够当分隔）；
         * 热点列不画 —— 那里第二行是刻度条，再叠一条线就是双线噪声。 */
        if (i + 1 < rows) v_rule(cr, x, ry + L_NW_ROW_H + pad - 1, cw);
    }
}

static void draw(cairo_t *cr, const view_ctx_t *v, const scene_env_t *e) {
    init_fonts();
    load();
    (void)e;

    const double mx = L_MARGIN, mw = v->lw - mx * 2, gap = L_COL_GAP;
    const double inner = mw - gap * 2;
    const double hotw = inner * L_NW_HOT_FRAC;
    const double techw = (inner - hotw) / 2;
    const double y0 = L_NW_Y0;

    /* 26 = 页脚高度（与面板同口径；可用高度从 lh 现算，换屏/换 scale 都不会压页脚） */
    double avail = v->lh - 26 - y0;
    if (avail < 0) avail = 0;

    /* 上屏条数：先算版面，再写顶栏 —— 顶栏报的必须是**真的排上了的条数**，
     * 不是缓存池大小（抓 20 条只展示 13 条时，写 20 会误导）。 */
    int per = (int)(avail / L_NW_ROW_H);
    if (per < 1) per = 1;
    int rows_hot = (int)((avail - L_NW_LEAD_H) / L_NW_ROW_H);
    if (rows_hot < 0) rows_hot = 0;
    if (rows_hot > nhot - 1) rows_hot = nhot - 1;
    int shown_hot = nhot > 0 ? rows_hot + 1 : 0;
    int half = (ntech + 1) / 2;
    if (half > per) half = per;
    int rest = ntech - half;
    if (rest < 0) rest = 0;
    if (rest > per) rest = per;
    int shown_tech = half + rest;

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
                 shown_hot, shown_tech);
    v_rule(cr, mx, L_RULE1_Y, mw);

    if (nhot == 0 && ntech == 0) {
        v_text_center(cr, v->lw / 2, v->lh / 2 - 14, v_fd_big(), C_OVERLAY0, "新闻还没同步");
        v_text_center(cr, v->lw / 2, v->lh / 2 + 20, v_fd_label(), C_OVERLAY0,
                      "systemctl start hpopi5b-news.service");
        v_text_center(cr, v->lw / 2, v->lh / 2 + 42, v_fd_small(), C_OVERLAY0, "%s", cache_path);
        return;
    }

    plan(hotw, techw);

    /* 三列：热点 1 列 + 科技 2 列；列与列之间一条竖线（没有列标题，分组靠位置与竖线） */
    const double tx1 = mx + hotw + gap;
    const double tx2 = tx1 + techw + gap;
    col_rule(cr, tx1 - gap / 2, y0 - 4, avail + 8);
    col_rule(cr, tx2 - gap / 2, y0 - 4, avail + 8);

    draw_hot(cr, mx, hotw, y0, avail);

    /* 科技条目在两个子列之间对半分：先排满左子列，再排右子列 */
    draw_tech(cr, tx1, techw, y0, avail, 0, half);
    draw_tech(cr, tx2, techw, y0, avail, half, ntech - half);

    if (getenv("LAYOUT_DEBUG"))
        fprintf(stderr,
                "news: hot=%d/%d tech=%d/%d hotw=%.1f techw=%.1f y0=%.1f avail=%.1f "
                "rows_hot=%d rows_tech=%d lead=%.1f row=%.1f H_ROW=%.1f lh=%.1f\n",
                shown_hot, nhot, shown_tech, ntech, hotw, techw, y0, avail, rows_hot, per,
                L_NW_LEAD_H, L_NW_ROW_H, H_ROW, v->lh);
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

const scene_t SCENE_NEWS = {"news", "新闻（国内热点 + 科技）", draw, sig, 0};