/* scene_trending.c —— 幕「GitHub 趋势」
 *
 * 数据由 systemd timer 每 3 小时抓一次（tools/fetch-trending.py），面板**只读缓存文件**：
 * 网络坏了就继续显示上一份，渲染永远不被网络阻塞（和 remote 采集同一个原则）。
 *
 * 为什么抓网页而不是 API：GitHub 没有 trending 的 API（详见 fetch-trending.py 顶部）。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "theme.h"
#include "scene.h"

/* 缓存里最多留 24 条；上屏条数不写死（由可用高度决定，GitHub 每日榜一般给 15~25） */
#define TR_MAX 24
#define TR_SHOW_MAX 24

typedef struct {
    char name[80];
    char lang[32];
    char today[16];
    char total[16];
    char desc[256];
} tr_row_t;

static tr_row_t rows[TR_MAX];
static int nrows;
static char cache_path[256] = "/var/cache/hpopi5b-panel/trending.tsv";
static struct stat lst;
static int loaded;

static PangoFontDescription *FD_REPO, *FD_TODAY, *FD_DESC, *FD_RANK, *FD_HEAD;
static double H_NAME, H_DESC; /* 实测字高（不拿字号乘系数赌）：首次绘制时量一次 */

static void init_fonts(void) {
    if (FD_REPO) return;
    FD_REPO = v_fd_proc();   /* 仓库名：比描述小一号，别喧宾夺主 */
    FD_TODAY = v_fd_label();
    FD_DESC = v_fd_desc();   /* 描述 13pt：两列后仍有足够空间 */
    FD_RANK = v_fd_small();
    FD_HEAD = v_fd_title();
    H_NAME = v_text_h(FD_REPO, "Ag");
    H_DESC = v_text_h(FD_DESC, "Ag");
}

void scene_trending_file(const char *p) {
    if (!p || !*p) return;
    snprintf(cache_path, sizeof cache_path, "%s", p);
}

/* 按真实像素宽度截断（不要用 v_truncate_mono：它按「字符数」算，CJK 会溢出） */
static void fit(char *out, size_t outn, PangoFontDescription *fd, const char *s, double maxw) {
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
        if (mid == lo) break; /* 多字节字符：再缩就退不回去了，到此为止 */
        char tmp[512];
        snprintf(tmp, sizeof tmp, "%.*s", mid, s);
        if (v_text_w(fd, tmp) <= budget) lo = mid; else hi = mid - 1;
    }
    while (lo > 0 && ((unsigned char)s[lo] & 0xC0) == 0x80) lo--;
    snprintf(out, outn, "%.*s…", lo, s);
}

static void load(void) {
    struct stat st;
    if (stat(cache_path, &st) != 0) {
        nrows = 0;
        loaded = 1;
        return;
    }
    if (loaded && st.st_mtime == lst.st_mtime && st.st_size == lst.st_size) return;
    lst = st;
    loaded = 1;
    nrows = 0;

    FILE *f = fopen(cache_path, "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof line, f) && nrows < TR_MAX) {
        if (line[0] == '#') continue;
        /* ⚠️ 必须先去掉行尾换行，再按 \t 切列：切列会把 \t 写成 NUL，
         *    strcspn 之后就停在第一个 NUL 上 ⇒ 行尾的 \n 永远去不掉。
         *    描述里带一个 \n 的后果：pango 认为它是**两行**，每条多占 24 像素、
         *    流式排版两列都排不满（这条坑 2026-11-06 踩过，量了半天才找到）。 */
        line[strcspn(line, "\r\n")] = 0;
        char *p = line;
        char *col[5];
        int nc = 0;
        col[nc++] = p;
        for (; *p && nc < 5; p++)
            if (*p == '\t') {
                *p = 0;
                col[nc++] = p + 1;
            }
        if (nc < 5) continue;
        tr_row_t *r = &rows[nrows];
        snprintf(r->name, sizeof r->name, "%.79s", col[0]);
        snprintf(r->lang, sizeof r->lang, "%.15s", col[1]);
        snprintf(r->today, sizeof r->today, "%s", col[2]);
        snprintf(r->total, sizeof r->total, "%s", col[3]);
        snprintf(r->desc, sizeof r->desc, "%s", col[4]);
        nrows++;
    }
    fclose(f);
}

/* "313,652" → "313.7k"（远端榜单给的是带千分位的整数） */
static void fmt_total(const char *s, char *out, size_t n) {
    long v = 0;
    for (; *s; s++)  /* 榜单给的是 "313,652"，atol 会在逗号处停下 */
        if (*s >= '0' && *s <= '9') v = v * 10 + (*s - '0');
    if (v >= 1000000) snprintf(out, n, "%.1fM", v / 1000000.0);
    else if (v >= 1000) snprintf(out, n, "%.1fk", v / 1000.0);
    else snprintf(out, n, "%ld", v);
}

/* "2,679" → 2679（今日涨星求和；只为本幕的抬头一行） */
static long today_of(const char *s) {
    long v = 0;
    for (; *s; s++)
        if (*s >= '0' && *s <= '9') v = v * 10 + (*s - '0');
    return v;
}

static void fmt_num(long v, char *out, size_t n) {
    if (v >= 1000000) snprintf(out, n, "%.1fM", v / 1000000.0);
    else if (v >= 1000) snprintf(out, n, "%.1fk", v / 1000.0);
    else snprintf(out, n, "%ld", v);
}

/* ---- 语言分布块（挤占网格最后一个格）：把「今天趋势偏哪些语言」压成一条堆叠条 ----
 * 为什么放这里：15 条上屏后 16 格差 1 格（GitHub 每日榜就是给 15~25 条），
 * 空着那格最显眼 —— 用**真实聚合**把它填上，比硬塞第 16 个仓库更有信息量。
 */
typedef struct {
    char name[32];
    int cnt;
} lang_cnt_t;

#define LTR_TOP 6 /* 最多列 6 种，剩下的归「其他」（图例才放得进一格） */

static int lang_agg(lang_cnt_t *out, int nrows_in) {
    lang_cnt_t all[24];
    int na = 0;
    for (int i = 0; i < nrows_in; i++) {
        const char *lg = rows[i].lang[0] ? rows[i].lang : "未知";
        int k = 0;
        for (; k < na; k++)
            if (!strcmp(all[k].name, lg)) break;
        if (k == na) {
            na++;
            snprintf(all[k].name, sizeof all[k].name, "%s", lg);
            all[k].cnt = 0;
        }
        all[k].cnt++;
    }
    /* 按条数降序（同数按名字，保证每次刷新颜色不乱跳） */
    for (int i = 0; i < na; i++)
        for (int j = i + 1; j < na; j++)
            if (all[j].cnt > all[i].cnt ||
                (all[j].cnt == all[i].cnt && strcmp(all[j].name, all[i].name) < 0)) {
                lang_cnt_t t = all[i];
                all[i] = all[j];
                all[j] = t;
            }
    int k = na < LTR_TOP ? na : LTR_TOP;
    memcpy(out, all, sizeof(lang_cnt_t) * (size_t)k);
    if (na > k) { /* 把尾巴合并成「其他」 */
        int rest = 0;
        for (int i = k; i < na; i++) rest += all[i].cnt;
        snprintf(out[k].name, sizeof out[k].name, "其他");
        out[k].cnt = rest;
        k++;
    }
    return k;
}

/* 堆叠条用**同一色相、alpha 递降**：这是一个「有序构成」，不是分类编码。
 * ⛔ 不要把语义色借给语言排行榜（绿=正常、红=危险，混用会让那套语义失效）；
 *    段的身份由图例的「名字 + 条数」确定，颜色只负责「大于小」。 */
static double seg_alpha(int i) {
    static const double A[LTR_TOP + 1] = {0.85, 0.66, 0.52, 0.40, 0.31, 0.25, 0.19};
    return A[i < 0 ? 0 : (i > LTR_TOP ? LTR_TOP : i)];
}

/* 语言分布：**一行文字 + 一条堆叠条**（高度压到 ~30 逻辑像素）。
 * 为什么不做「色块图例」：7 种语言配 7 个色块标不下一行（实测 ~670 > 617 放不下），
 * 换行就要再吃 16 像素 —— 而这一块是列表的脚注，不是主角。
 * 段的身份由**同一行里的「语言 N」按条数降序**给出，条上的 alpha 也从左到右递降，一一对应。 */
/* ---- 排版计划缓存 ------------------------------------------------------------
 * 行高只跟「描述文本 + 列宽」有关，两者在缓存文件不变时都不变；但面板闲时也在 **1 Hz 重画**
 * （趋势幕也一样）。不缓存的话每次重画都要量 15 段长描述 = 实测 +1.5% of one core（2.4% → 4.0%）。
 * 缓存键：列宽 + 缓存文件 mtime/size + 条数。文件一换（timer 每 3h）就重量一次。
 */
static struct {
    double cw;
    time_t mtime;
    off_t size;
    int n;
    double h[TR_SHOW_MAX];
    char langtxt[256];
    /* 每条第一行的三段文字也预先排好。名字要二分截断（最长 8 次整形），
     * 1 Hz 重画下每帧重算 = 白白多整 100+ 次形，实测每帧 +9.5ms（19.5 → 29.0）。
     * ⚠️ 这条和 view.c 里「别先量再画」是同一条纪律：**能缓存的排版就不要每帧排**。 */
    char name[TR_SHOW_MAX][96];
    char lang[TR_SHOW_MAX][64];
    char today[TR_SHOW_MAX][40];
    double today_w[TR_SHOW_MAX];
    double lang_w[TR_SHOW_MAX];
} plan;

/* 「语言分布 · 15 个仓库   Python 6 · Rust 3 · …」这一行：**只在数据变化时拼一次**
 * （拼的过程要逐项试宽，每项一次 pango 整形；1 Hz 重画下这么干是纯浪费）。 */
static void lang_line(char *out, size_t outn, double maxw, int shown) {
    lang_cnt_t lg[LTR_TOP + 1];
    int nl = lang_agg(lg, shown);
    int total = 0;
    for (int i = 0; i < nl; i++) total += lg[i].cnt;
    out[0] = 0;
    if (total <= 0) return;
    int off = snprintf(out, outn, "语言分布 · %d 个仓库", total);
    for (int i = 0; i < nl; i++) {
        int add = snprintf(out + off, outn - (size_t)off, "   %s %d", lg[i].name, lg[i].cnt);
        if (add < 0 || off + add >= (int)outn || v_text_w(v_fd_small(), out) > maxw) {
            out[off] = 0; /* 这一种放不下就收尾，绝不溢出到列外 */
            break;
        }
        off += add;
    }
}

static void draw_lang_block(cairo_t *cr, double cx, double y, double cw, int shown) {
    lang_cnt_t lg[LTR_TOP + 1];
    int nl = lang_agg(lg, shown);
    int total = 0;
    for (int i = 0; i < nl; i++) total += lg[i].cnt;
    if (total <= 0) return;

    v_text(cr, cx, y + 1, v_fd_small(), C_OVERLAY0, "%s", plan.langtxt);

    /* 堆叠条：按条数比例分宽（不画圆角：设计系统里没有卡片） */
    const double by = y + L_TR_LANG_TXT_H + 3, bh = 8;
    double x = cx;
    for (int i = 0; i < nl; i++) {
        double full = cw * (double)lg[i].cnt / total;
        double w = full;
        if (i + 1 < nl && w > 3) w -= 2; /* 段间 2px 缝：同色相接才不会糊成一段 */
        if (w > 0) {
            v_set_rgb(cr, C_MAUVE, seg_alpha(i));
            cairo_rectangle(cr, x, by, w, bh);
            cairo_fill(cr);
        }
        x += full;
    }
}

static void draw(cairo_t *cr, const view_ctx_t *v, const scene_env_t *e) {
    init_fonts();
    load();
    (void)e;

    const double mx = L_MARGIN, mw = v->lw - mx * 2;

    v_text(cr, mx, L_TOP_Y + 13, FD_HEAD, C_MAUVE, "GitHub 趋势 · 每日");
    v_rule(cr, mx, L_RULE1_Y, mw);

    if (nrows == 0) {
        v_text_center(cr, v->lw / 2, v->lh / 2 - 24, v_fd_big(), C_OVERLAY0, "GitHub 趋势还没同步");
        v_text_center(cr, v->lw / 2, v->lh / 2 + 12, v_fd_label(), C_OVERLAY0,
                      "systemctl start hpopi5b-trending.service");
        v_text_center(cr, v->lw / 2, v->lh / 2 + 34, v_fd_small(), C_OVERLAY0, "%s", cache_path);
        return;
    }

    int n = nrows < TR_SHOW_MAX ? nrows : TR_SHOW_MAX;
    const double gap = L_COL_GAP;
    const double cw = (mw - gap) / 2;
    const double y0 = L_TR_Y0;

    char head[64];
    long age = (long)(time(NULL) - lst.st_mtime);
    if (age < 90) snprintf(head, sizeof head, "刚刚同步");
    else if (age < 3600) snprintf(head, sizeof head, "%ld 分钟前同步", age / 60);
    else snprintf(head, sizeof head, "%ld 小时前同步", age / 3600);

    /* 今日涨星合计（全部条目，不只是上屏的）：把「一屏热不热」压缩成抬头一个数 */
    long sum = 0;
    for (int i = 0; i < nrows; i++) sum += today_of(rows[i].today);
    char sumtxt[24];
    fmt_num(sum, sumtxt, sizeof sumtxt);
    v_text_right(cr, v->lw - mx, L_TOP_Y + 13, v_fd_label(), C_OVERLAY0, "%s · 共 %d 条 · 今日 +%s★",
                 head, nrows, sumtxt);

    /* ---- 流式两列：行高随描述行数变，两列按总高度均衡分列 ----
     * 为什么不用固定网格：描述有的是 1 行、有的是 2 行，固定行距只能按「最宽的那个」留空，
     * 或者按「最窄的」挤死（8×79.5 网格里第二行描述就差 1 像素碰到行线）。 */
    const double avail = v->lh - 26 - y0; /* 26 = 页脚高度（与面板同口径） */
    double h[TR_SHOW_MAX];
    double pre[TR_SHOW_MAX + 1];
    const double descw = cw - L_TR_RANK_W;
    if (plan.cw != descw || plan.mtime != lst.st_mtime || plan.size != lst.st_size || plan.n != n) {
        plan.cw = descw;
        plan.mtime = lst.st_mtime;
        plan.size = lst.st_size;
        plan.n = n;
        for (int i = 0; i < n; i++) {
            double dh = v_text_measure(FD_DESC, descw, L_TR_DESC_LINES, rows[i].desc);
            if (dh <= 0) dh = H_DESC;
            plan.h[i] = 1 + H_NAME + L_TR_NAME_GAP + dh + L_TR_ROW_GAP;

            char tot_s[24];
            fmt_total(rows[i].total, tot_s, sizeof tot_s);
            snprintf(plan.today[i], sizeof plan.today[i], "+%s", rows[i].today);
            if (rows[i].lang[0])
                snprintf(plan.lang[i], sizeof plan.lang[i], "%s · %s★", rows[i].lang, tot_s);
            else snprintf(plan.lang[i], sizeof plan.lang[i], "%s★", tot_s);
            plan.today_w[i] = v_text_w(FD_TODAY, plan.today[i]) + 14;
            plan.lang_w[i] = v_text_w(FD_DESC, plan.lang[i]) + 14;
            fit(plan.name[i], sizeof plan.name[i], FD_REPO, rows[i].name,
                cw - L_TR_RANK_W - plan.today_w[i] - plan.lang_w[i]);
        }
        lang_line(plan.langtxt, sizeof plan.langtxt, cw, n);
    }
    for (int i = 0; i < n; i++) h[i] = plan.h[i];
    pre[0] = 0;
    for (int i = 0; i < n; i++) pre[i + 1] = pre[i] + h[i];
    const double tot = pre[n] + L_TR_LANG_H;

    /* 分列点：让左列高度尽量接近「总高的一半」（语言块算在右列尾部） */
    int split = n;
    if (n > 1) {
        double best = 1e9;
        for (int k = 1; k < n; k++) {
            double hl = pre[k], hr = tot - pre[k];
            double d = hl > hr ? hl - hr : hr - hl;
            if (d < best) {
                best = d;
                split = k;
            }
        }
    }

    /* 每列剩下的零头**均摊到行距里**（上限 8 像素/行）：两列都贴着屏幕底边收尾，
     * 不会出现「内容在 985、屏底在 1041」那种说不清的 56 像素空白。
     * ⚠️ 必须封顶：条目少的时候不封顶 = 回到「行距 127、一行只用掉 70」的老问题。 */
    double pad[2];
    for (int c = 0; c < 2; c++) {
        double used = c == 0 ? pre[split] : pre[n] - pre[split];
        int cnt = c == 0 ? split : n - split;
        if (c == 1) used += L_TR_LANG_H;
        double extra = avail - used;
        pad[c] = cnt > 0 ? extra / cnt : 0;
        if (pad[c] < 0) pad[c] = 0;
        if (pad[c] > L_TR_ROW_PAD_MAX) pad[c] = L_TR_ROW_PAD_MAX;
    }

    double cy[2] = {y0, y0};
    int idx_in_col[2] = {0, 0};
    for (int i = 0; i < n; i++) {
        int c = i < split ? 0 : 1;
        double cx = mx + c * (cw + gap);
        if (cy[c] + h[i] > y0 + avail) {
            idx_in_col[c]++;
            continue; /* 放不下就不画：宁可少一条，也不压页脚 */
        }
        tr_row_t *r = &rows[i];
        double y = cy[c];
        double tx = cx + L_TR_RANK_W;

        v_text(cr, cx, y + 1, FD_RANK, C_OVERLAY0, "%02d", i + 1);

        /* 第一行：仓库名（已截断到「语言 + 总星 + 今日涨星」之前） + 语言 · 总星 + 今日涨星
         * 三段都在计划缓存里排好了（见 plan），这里只画不量。 */
        v_text(cr, tx, y + 1, FD_REPO, C_TEXT, "%s", plan.name[i]);
        v_text_right(cr, cx + cw - plan.today_w[i], y + 2, FD_DESC, C_SUBTEXT1, "%s",
                     plan.lang[i]);
        v_text_right(cr, cx + cw, y + 1, FD_TODAY, C_YELLOW, "%s", plan.today[i]);

        /* 描述最多两行（超出省略），可用整列宽；行高已按实际行数量过 */
        v_text_wrap(cr, tx, y + 1 + H_NAME + L_TR_NAME_GAP, FD_DESC, C_SUBTEXT0, descw,
                    L_TR_DESC_LINES, "%s", r->desc);

        if (i + 1 < n) v_rule(cr, cx, y + h[i] - L_TR_RULE_B, cw);
        cy[c] = y + h[i] + pad[c];
        idx_in_col[c]++;
    }

    /* 尾部：语言分布（右列最后一行的下面；实在放不下就不画，不压页脚） */
    if (cy[1] + L_TR_LANG_H <= y0 + avail) draw_lang_block(cr, mx + cw + gap, cy[1] + 4, cw, n);

    if (getenv("LAYOUT_DEBUG"))
        fprintf(stderr,
                "trending: n=%d split=%d y0=%.1f avail=%.1f total=%.1f left=%.1f right=%.1f "
                "H_NAME=%.1f H_DESC=%.1f lh=%.1f\n",
                n, split, y0, avail, tot, cy[0] - y0, cy[1] - y0, H_NAME, H_DESC, v->lh);
}

/* 缓存文件变了 / 每分钟一次的相对时间需要重画 */
static unsigned long long sig(const scene_env_t *e) {
    load();
    unsigned long long h = 1469598103934665603ULL;
    h = (h ^ (unsigned long long)lst.st_mtime) * 1099511628211ULL;
    h = (h ^ (unsigned long long)nrows) * 1099511628211ULL;
    h = (h ^ (unsigned long long)((long)e->wall / 60)) * 1099511628211ULL;
    return h;
}

const scene_t SCENE_TRENDING = {"trending", "GitHub 趋势", draw, sig, 0};