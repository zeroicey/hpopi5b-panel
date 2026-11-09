/* scene_weather.c —— 幕「天气」：主城（广东东软学院）+ 周边/关注城市
 *
 * 用户口径（2026-11-09）：「想做一个新一幕，天气相关的，显示最近天气，我所在的位置在
 * 广东东软学院」「需不需加多几个地区占满空位」。
 *   ⇒ 版式 B（用户选的）：上半屏 左 61.5% 主城（44pt 大温度 + 未来 24 小时温度曲线 +
 *     降水概率条 + 日出日落/AQI），右 38.5% 未来 7 天；底部一条「关注城市」4 列
 *     （东莞常平 / 湖北利川 / 广州 / 深圳）。
 *
 * 数据通路（与 trending / news 完全同一条，面板自己从不联网）：
 *   tools/fetch-weather.py（Open-Meteo 预报 + 空气质量，免 key）
 *        └─ 原子写 /var/cache/hpopi5b-panel/weather.tsv   ← systemd timer 每 10 分钟
 *   scene_weather.c  stat() 比对 mtime/size，文件变了才重读；渲染永远不被网络阻塞。
 *
 * 诚实原则：抓不到就继续显示上一份，右上角写「N 分钟前同步」，超过 30 分钟（3 个抓取
 * 周期）转黄；从来没有成功过就画空态（服务名 + 缓存路径），**不灌假数据、不预填**
 * （AGENTS.md 第 5/7 节）。
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "theme.h"
#include "scene.h"

#define WX_CITIES 8   /* 缓存里最多认几城；上屏只画前 4 个「关注城市」 */
#define WX_HOURS  24
#define WX_DAYS   7

typedef struct {
    int id;
    char name[48];
    double temp, feels, rh, code;
    double wind, wind_dir, gust;
    double precip, cloud, pressure;
    double tmax, tmin, pp_max;
    double aqi, pm25, uv;
} wx_city_t;

typedef struct {
    int hour;       /* 0~23，从 H 行时间戳里切出来 */
    double temp, feels, pp, precip, code, wind;
} wx_hour_t;

typedef struct {
    char date[12];  /* YYYY-MM-DD */
    double code, tmax, tmin, pp_max, precip_sum, uv_max;
    char sunrise[8], sunset[8];  /* HH:MM */
} wx_day_t;

static wx_city_t city[WX_CITIES];
static wx_hour_t hour[WX_CITIES][WX_HOURS];
static wx_day_t day[WX_CITIES][WX_DAYS];
static int ncity, nh[WX_CITIES], nd[WX_CITIES];
static char campus[64] = "广东东软学院";
static char cache_path[256] = "/var/cache/hpopi5b-panel/weather.tsv";
static struct stat lst;
static int loaded;

static PangoFontDescription *FD_HEAD, *FD_LABEL, *FD_SMALL, *FD_DESC;

static void init_fonts(void) {
    if (FD_HEAD) return;
    FD_HEAD = v_fd_label();   /* 顶栏：10pt，别抢正文 */
    FD_LABEL = v_fd_label();
    FD_SMALL = v_fd_small();  /* 9pt：辅助信息 */
    FD_DESC = v_fd_desc();    /* 13pt：天气词 */
}

void scene_weather_file(const char *p) {
    if (!p || !*p) return;
    snprintf(cache_path, sizeof cache_path, "%s", p);
}

/* ---------------- 解析 ---------------- */
static double num(const char *s) {
    if (!s || !*s) return -1;
    char *e = NULL;
    double v = strtod(s, &e);
    if (e == s) return -1;
    return v;
}

static void load(void) {
    struct stat st;
    if (stat(cache_path, &st) != 0) {
        if (!loaded) { ncity = 0; loaded = 1; }
        return;
    }
    if (loaded && st.st_mtime == lst.st_mtime && st.st_size == lst.st_size) return;
    lst = st;
    loaded = 1;
    ncity = 0;
    memset(city, 0, sizeof city);
    memset(nh, 0, sizeof nh);
    memset(nd, 0, sizeof nd);

    FILE *f = fopen(cache_path, "r");
    if (!f) return;
    char line[2048];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        line[strcspn(line, "\r\n")] = 0;
        char *col[32];
        int nc = 0;
        char *p = line;
        col[nc++] = p;
        for (; *p && nc < 32; p++)
            if (*p == '\t') { *p = 0; col[nc++] = p + 1; }
        if (nc < 2) continue;

        if (strcmp(col[0], "P") == 0 && nc >= 3) {
            snprintf(campus, sizeof campus, "%s", col[1]);
        } else if (strcmp(col[0], "C") == 0 && nc >= 19) {
            int id = (int)num(col[1]);
            if (id < 0 || id >= WX_CITIES) continue;
            wx_city_t *c = &city[id];
            memset(c, 0, sizeof *c);
            c->id = id;
            snprintf(c->name, sizeof c->name, "%s", col[2]);
            c->temp = num(col[3]);
            c->feels = num(col[4]);
            c->rh = num(col[5]);
            c->code = num(col[6]);
            c->wind = num(col[7]);
            c->wind_dir = num(col[8]);
            c->gust = num(col[9]);
            c->precip = num(col[10]);
            c->cloud = num(col[11]);
            c->pressure = num(col[12]);
            c->tmax = num(col[13]);
            c->tmin = num(col[14]);
            c->pp_max = num(col[15]);
            c->aqi = num(col[16]);
            c->pm25 = num(col[17]);
            c->uv = num(col[18]);
            if (id + 1 > ncity) ncity = id + 1;
        } else if (strcmp(col[0], "H") == 0 && nc >= 8) {
            int id = (int)num(col[1]);
            if (id < 0 || id >= WX_CITIES || nh[id] >= WX_HOURS) continue;
            wx_hour_t *h = &hour[id][nh[id]];
            h->hour = (strlen(col[2]) >= 13) ? atoi(col[2] + 11) : -1;
            h->temp = num(col[3]);
            h->feels = num(col[4]);
            h->pp = num(col[5]);
            h->precip = num(col[6]);
            h->code = num(col[7]);
            h->wind = num(col[8]);
            nh[id]++;
        } else if (strcmp(col[0], "D") == 0 && nc >= 11) {
            int id = (int)num(col[1]);
            if (id < 0 || id >= WX_CITIES || nd[id] >= WX_DAYS) continue;
            wx_day_t *d = &day[id][nd[id]];
            snprintf(d->date, sizeof d->date, "%s", col[2]);
            d->code = num(col[3]);
            d->tmax = num(col[4]);
            d->tmin = num(col[5]);
            d->pp_max = num(col[6]);
            d->precip_sum = num(col[7]);
            snprintf(d->sunrise, sizeof d->sunrise, "%s", col[8]);
            snprintf(d->sunset, sizeof d->sunset, "%s", col[9]);
            d->uv_max = num(col[10]);
            nd[id]++;
        }
    }
    fclose(f);
}

static unsigned long long sig(const scene_env_t *e) {
    load();
    unsigned long long h = 1469598103934665603ULL;
    h = (h ^ (unsigned long long)lst.st_mtime) * 1099511628211ULL;
    h = (h ^ (unsigned long long)lst.st_size) * 1099511628211ULL;
    h = (h ^ (unsigned long long)(ncity * 64 + nh[0])) * 1099511628211ULL;
    /* 「N 分钟前同步」每分钟都要变；不混 mtime 之外的时间，避免每秒重绘（live=0） */
    h = (h ^ (unsigned long long)((long)e->wall / 60)) * 1099511628211ULL;
    return h;
}

/* ---------------- 文案 / 颜色 ---------------- */
static const char *wmo_text(double c) {
    int v = (int)lround(c);
    switch (v) {
    case 0: return "晴";
    case 1: return "大部晴";
    case 2: return "多云";
    case 3: return "阴";
    case 45: case 48: return "雾";
    case 51: case 53: case 55: return "毛毛雨";
    case 56: case 57: return "冻雨";
    case 61: return "小雨";
    case 63: return "中雨";
    case 65: return "大雨";
    case 66: case 67: return "冻雨";
    case 71: return "小雪";
    case 73: return "中雪";
    case 75: return "大雪";
    case 77: return "米雪";
    case 80: case 81: return "阵雨";
    case 82: return "强阵雨";
    case 85: case 86: return "阵雪";
    case 95: return "雷阵雨";
    case 96: case 99: return "雷暴冰雹";
    default: return "—";
    }
}

static uint32_t wmo_col(double c) {
    int v = (int)lround(c);
    if (v == 0 || v == 1) return C_PEACH;                 /* 晴：暖色 */
    if (v == 2 || v == 3 || v == 45 || v == 48) return C_OVERLAY1;
    if ((v >= 71 && v <= 77) || v == 85 || v == 86) return C_LAVENDER;
    if (v >= 95) return C_RED;
    if (v >= 51) return C_BLUE;                           /* 雨：蓝 */
    return C_OVERLAY1;
}

static int beaufort(double kmh) {
    static const double b[] = {1, 6, 12, 20, 29, 39, 50, 62, 75, 89, 103, 118};
    int lv = 0;
    for (int i = 0; i < 12; i++)
        if (kmh >= b[i]) lv = i + 1;
    return lv;
}

static const char *dir_word(double deg) {
    static const char *n[] = {"北风", "东北风", "东风", "东南风", "南风", "西南风", "西风", "西北风"};
    int i = (int)((deg + 22.5) / 45.0) % 8;
    if (i < 0) i += 8;
    return n[i];
}

static const char *aqi_word(double a) {
    if (a < 0) return "无数据";
    if (a <= 50) return "优";
    if (a <= 100) return "良";
    if (a <= 150) return "轻度污染";
    if (a <= 200) return "中度污染";
    if (a <= 300) return "重度污染";
    return "严重污染";
}

static uint32_t aqi_col(double a) {
    if (a < 0) return C_OVERLAY0;
    if (a <= 50) return C_GREEN;
    if (a <= 100) return C_YELLOW;
    if (a <= 150) return C_PEACH;
    if (a <= 200) return C_RED;
    return C_LAVENDER;
}

/* 公历 Y-M-D → 自 1970-01-01 的天数（Howard Hinnant 算法；只用来算星期/今天明天） */
static long days_from_civil(int y, int m, int d) {
    y -= (m <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (long)era * 146097L + (long)doe - 719468L;
}

static long date_days(const char *s) {
    int y = 0, m = 0, d = 0;
    if (sscanf(s, "%d-%d-%d", &y, &m, &d) != 3) return -999999L;
    return days_from_civil(y, m, d);
}

static void day_label(long d, long today, char *out, size_t n) {
    static const char *wk[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    long diff = d - today;
    if (diff == 0) { snprintf(out, n, "今天"); return; }
    if (diff == 1) { snprintf(out, n, "明天"); return; }
    int w = (int)((d + 4) % 7);
    if (w < 0) w += 7;
    snprintf(out, n, "%s", wk[w]);
}

static double hm2h(const char *s) {
    int h = 0, m = 0;
    if (sscanf(s, "%d:%d", &h, &m) != 2) return -1;
    return h + m / 60.0;
}

/* ---------------- 绘制 ---------------- */
static void vrule(cairo_t *cr, double x, double y, double h) {
    cairo_rectangle(cr, x, y, 1.0, h);
    v_set_rgb(cr, C_SURFACE1, 0.32);
    cairo_fill(cr);
}

static void draw_curve(cairo_t *cr, double x, double y, double w, double h,
                       const wx_hour_t *hs, int n) {
    double lo = 1e9, hi = -1e9;
    for (int i = 0; i < n; i++) {
        if (hs[i].temp < -100) continue;
        if (hs[i].temp < lo) lo = hs[i].temp;
        if (hs[i].temp > hi) hi = hs[i].temp;
    }
    if (lo > hi) return;
    double range = hi - lo;
    if (range < 4.0) { double mid = (hi + lo) / 2; lo = mid - 2; hi = mid + 2; range = 4; }
    lo -= range * 0.08;
    hi += range * 0.08;

    /* 三条极淡刻度线：让曲线区读作「一张图」，空的部分是刻度之间的留白 */
    for (int i = 1; i <= 3; i++) {
        cairo_rectangle(cr, x, y + h - h * (double)i / 4.0, w, 1.0);
        v_set_rgb(cr, C_SURFACE1, 0.30);
        cairo_fill(cr);
    }

    double step = n > 1 ? w / (double)(n - 1) : 0;
#define WX_Y(v) (y + h - ((v) - lo) / (hi - lo) * h)
    /* 面积填充（很淡：曲线才是主体，底色块不能变成卡片） */
    int first = -1, last = -1;
    for (int i = 0; i < n; i++)
        if (hs[i].temp > -100) { if (first < 0) first = i; last = i; }
    if (first < 0) return;
    cairo_move_to(cr, x + step * first, WX_Y(hs[first].temp));
    for (int i = first + 1; i <= last; i++)
        if (hs[i].temp > -100) cairo_line_to(cr, x + step * i, WX_Y(hs[i].temp));
    cairo_line_to(cr, x + step * last, y + h);
    cairo_line_to(cr, x + step * first, y + h);
    cairo_close_path(cr);
    v_set_rgb(cr, C_PEACH, 0.08);
    cairo_fill(cr);

    /* 折线 */
    int started = 0;
    for (int i = 0; i < n; i++) {
        if (hs[i].temp <= -100) continue;
        if (!started) { cairo_move_to(cr, x + step * i, WX_Y(hs[i].temp)); started = 1; }
        else cairo_line_to(cr, x + step * i, WX_Y(hs[i].temp));
    }
    v_set_rgb(cr, C_PEACH, 1.0);
    cairo_set_line_width(cr, 2.0);
    cairo_stroke(cr);

    /* 每 6 小时一个点 + 跨天竖细线 */
    for (int i = 0; i < n; i++) {
        if (hs[i].temp <= -100) continue;
        if (i == 0 || i == 6 || i == 12 || i == 18 || i == n - 1) {
            cairo_arc(cr, x + step * i, WX_Y(hs[i].temp), 2.4, 0, 2 * M_PI);
            v_set_rgb(cr, C_PEACH, 1.0);
            cairo_fill(cr);
        }
    }
    /* 跨天位置画一条淡竖线（0 点） */
    for (int i = 1; i < n; i++) {
        if (hs[i].hour == 0) {
            cairo_rectangle(cr, x + step * i, y, 1.0, h);
            v_set_rgb(cr, C_SURFACE1, 0.22);
            cairo_fill(cr);
        }
    }
#undef WX_Y
}

static void draw_precip(cairo_t *cr, double x, double y, double w, double h,
                        const wx_hour_t *hs, int n) {
    /* y 是标题行顶部；条从 y+16 开始 */
    double top = y + 16;
    cairo_rectangle(cr, x, top + h, w, 1.0);
    v_set_rgb(cr, C_SURFACE1, 0.35);
    cairo_fill(cr);
    int any = 0;
    for (int i = 0; i < n; i++)
        if (hs[i].pp >= 1) any = 1;
    if (!any) {
        v_text_center(cr, x + w / 2, top + h / 2 - 9, FD_SMALL, C_OVERLAY0,
                      "未来 24 小时降水概率均低于 5%%");
        return;
    }
    double step = n > 0 ? w / (double)n : 0;
    for (int i = 0; i < n; i++) {
        if (hs[i].pp < 0) continue;
        double bw = step * 0.55;
        if (bw < 2) bw = 2;
        double bh = h * (hs[i].pp / 100.0);
        if (bh < 0.5 && hs[i].pp > 0) bh = 0.5;
        cairo_rectangle(cr, x + step * i + (step - bw) / 2, top + h - bh, bw, bh);
        v_set_rgb(cr, C_BLUE, hs[i].pp >= 50 ? 0.75 : 0.45);
        cairo_fill(cr);
    }
}

static void draw_sun(cairo_t *cr, double x, double y, const wx_city_t *m,
                     const wx_day_t *d, const scene_env_t *e) {
    double sr = hm2h(d->sunrise), ss = hm2h(d->sunset);
    double line_y = y + 34;
    v_text(cr, x, y, FD_SMALL, C_OVERLAY1, "日出 %s", d->sunrise[0] ? d->sunrise : "--:--");
    v_text_right(cr, x + L_WX_SUN_W, y, FD_SMALL, C_OVERLAY1, "日落 %s",
                 d->sunset[0] ? d->sunset : "--:--");
    cairo_rectangle(cr, x, line_y, L_WX_SUN_W, 1.0);
    v_set_rgb(cr, C_SURFACE1, 0.45);
    cairo_fill(cr);
    struct tm tm;
    localtime_r(&e->wall, &tm);
    double nowh = tm.tm_hour + tm.tm_min / 60.0;
    int night = (sr < 0 || ss <= sr || nowh < sr || nowh > ss);
    double frac = (ss > sr) ? (nowh - sr) / (ss - sr) : 0;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    cairo_arc(cr, x + frac * L_WX_SUN_W, line_y + 0.5, 3.2, 0, 2 * M_PI);
    v_set_rgb(cr, night ? C_OVERLAY1 : C_YELLOW, 1.0);
    cairo_fill(cr);
    v_text(cr, x, y + 42, FD_SMALL, C_OVERLAY0, "现在 %02d:%02d · %s", tm.tm_hour, tm.tm_min,
           night ? "夜间" : "白天");
    (void)m;
}

static void draw_aqi(cairo_t *cr, double x, double y, const wx_city_t *m) {
    v_text(cr, x, y, FD_LABEL, aqi_col(m->aqi), "空气质量 %s", aqi_word(m->aqi));
    if (m->aqi < 0)
        v_text(cr, x, y + 22, FD_SMALL, C_OVERLAY0, "空气质量接口暂时没有数据");
    else
        v_text(cr, x, y + 22, FD_SMALL, C_OVERLAY0, "AQI %.0f · PM2.5 %.1f µg/m³ · 紫外线 %.1f",
               m->aqi, m->pm25, m->uv);
    /* 一条 0~300 的浅刻度：让「60」有位置感，不是孤立数字 */
    double by = y + 46, bw = 180;
    cairo_rectangle(cr, x, by, bw, 2.5);
    v_set_rgb(cr, C_SURFACE1, 0.30);
    cairo_fill(cr);
    double fx = m->aqi / 300.0;
    if (fx < 0) fx = 0;
    if (fx > 1) fx = 1;
    cairo_rectangle(cr, x + fx * bw, by - 1.5, 2.5, 5.5);
    v_set_rgb(cr, aqi_col(m->aqi), 1.0);
    cairo_fill(cr);
}

static void draw_days(cairo_t *cr, double x, double y, double w, long today) {
    v_text(cr, x, y, FD_LABEL, C_OVERLAY1, "未来 7 天");
    v_text_right(cr, x + w, y, FD_SMALL, C_OVERLAY0, "降水概率");
    if (nd[0] <= 0) {
        v_text(cr, x, y + 30, FD_LABEL, C_OVERLAY0, "7 天预报还没拿到");
        return;
    }
    double wmin = 1e9, wmax = -1e9;
    for (int i = 0; i < nd[0]; i++) {
        if (day[0][i].tmin > -100) { if (day[0][i].tmin < wmin) wmin = day[0][i].tmin; }
        if (day[0][i].tmax > -100) { if (day[0][i].tmax > wmax) wmax = day[0][i].tmax; }
    }
    if (wmin > wmax) { wmin = 0; wmax = 1; }
    if (wmax - wmin < 1) wmax = wmin + 1;

    double rows_y = y + 24;
    for (int i = 0; i < nd[0]; i++) {
        wx_day_t *d = &day[0][i];
        double ry = rows_y + i * L_WX_DAY_ROW_H;
        char lb[16];
        day_label(date_days(d->date), today, lb, sizeof lb);
        v_text(cr, x, ry + 4, FD_LABEL, i == 0 ? C_MAUVE : C_TEXT, "%s", lb);
        v_text(cr, x + L_WX_DAY_NAME_W, ry + 1, FD_DESC, wmo_col(d->code), "%s", wmo_text(d->code));
        v_text_right(cr, x + w, ry + 4, FD_LABEL, C_TEXT, "%.0f° / %.0f°", d->tmax, d->tmin);

        double bx = x + L_WX_DAY_NAME_W + L_WX_DAY_COND_W;
        double bz = w - L_WX_DAY_NAME_W - L_WX_DAY_COND_W - 86;
        if (bz < 40) bz = 40;
        cairo_rectangle(cr, bx, ry + 34, bz, L_WX_BAR_H);
        v_set_rgb(cr, C_SURFACE1, 0.45);
        cairo_fill(cr);
        double x1 = bx + (d->tmin - wmin) / (wmax - wmin) * bz;
        double x2 = bx + (d->tmax - wmin) / (wmax - wmin) * bz;
        if (x2 - x1 < 3) x2 = x1 + 3;
        cairo_rectangle(cr, x1, ry + 34, x2 - x1, L_WX_BAR_H);
        v_set_rgb(cr, i == 0 ? C_MAUVE : C_PEACH, i == 0 ? 1.0 : 0.75);
        cairo_fill(cr);

        v_text(cr, bx, ry + 44, FD_SMALL, C_BLUE, "降水 %.0f%%", d->pp_max);
        if (d->precip_sum >= 0)
            v_text_right(cr, x + w, ry + 44, FD_SMALL, C_OVERLAY0, "%.1f mm", d->precip_sum);
        if (i + 1 < nd[0]) v_rule(cr, x, ry + L_WX_DAY_ROW_H - 4, w);
    }
}

static void draw_city_strip(cairo_t *cr, double mx, double mw, double band_top, long today) {
    (void)today;
    v_rule(cr, mx, band_top, mw);
    v_text(cr, mx, band_top + 10, FD_SMALL, C_OVERLAY0, "关注城市");
    int nc = ncity > 1 ? ncity - 1 : 0;
    if (nc > 4) nc = 4;
    if (nc <= 0) return;
    double x0 = mx + L_WX_CITY_LABEL_W;
    double colw = (mw - L_WX_CITY_LABEL_W) / (double)nc;

    /* 统一量程：4 个城市共用一条温度轴，条长才可比（否则每列都「满格」= 没信息） */
    double gmin = 1e9, gmax = -1e9;
    for (int i = 1; i <= nc; i++) {
        if (city[i].tmin > -100 && city[i].tmin < gmin) gmin = city[i].tmin;
        if (city[i].tmax > -100 && city[i].tmax > gmax) gmax = city[i].tmax;
    }
    if (gmin > gmax) { gmin = 0; gmax = 1; }
    if (gmax - gmin < 1) gmax = gmin + 1;

    for (int i = 1; i <= nc; i++) {
        wx_city_t *c = &city[i];
        double cx = x0 + (i - 1) * colw;
        if (i > 1) vrule(cr, cx - 12, band_top + 8, L_WX_CITY_H - 16);
        v_text(cr, cx, band_top + 8, FD_LABEL, C_OVERLAY1, "%s", c->name);
        char tb[24];
        snprintf(tb, sizeof tb, "%.1f°", c->temp);
        v_text(cr, cx, band_top + 24, v_fd_big(), C_TEXT, "%s", tb);
        double tw = v_text_w(v_fd_big(), tb);
        v_text(cr, cx + tw + 12, band_top + 34, FD_DESC, wmo_col(c->code), "%s",
               wmo_text(c->code));
        v_text(cr, cx, band_top + 62, FD_SMALL, C_OVERLAY0, "%.0f° / %.0f° · 降水 %.0f%%",
               c->tmax, c->tmin, c->pp_max);
        double bz = 110;
        double bx = cx;
        cairo_rectangle(cr, bx, band_top + 80, bz, L_WX_BAR_H);
        v_set_rgb(cr, C_SURFACE1, 0.45);
        cairo_fill(cr);
        double x1 = bx + (c->tmin - gmin) / (gmax - gmin) * bz;
        double x2 = bx + (c->tmax - gmin) / (gmax - gmin) * bz;
        if (x2 - x1 < 3) x2 = x1 + 3;
        cairo_rectangle(cr, x1, band_top + 80, x2 - x1, L_WX_BAR_H);
        v_set_rgb(cr, C_PEACH, 0.75);
        cairo_fill(cr);
    }
}

static void draw(cairo_t *cr, const view_ctx_t *v, const scene_env_t *e) {
    init_fonts();
    load();

    const double mx = L_MARGIN, mw = v->lw - mx * 2;
    const double leftw = mw * L_WX_MAIN_FRAC;
    const double rightx = mx + leftw + L_COL_GAP;
    const double rightw = mw - leftw - L_COL_GAP;
    const double band_top = v->lh - 26 - L_WX_CITY_H;

    v_text(cr, mx, L_TOP_Y + 13, FD_HEAD, C_MAUVE, "天气");

    long age = lst.st_mtime ? (long)(e->wall - lst.st_mtime) : -1;
    char head[64];
    uint32_t hc = C_OVERLAY0;
    if (!loaded || lst.st_mtime == 0) snprintf(head, sizeof head, "还没同步过");
    else if (age < 90) snprintf(head, sizeof head, "刚刚同步");
    else if (age < 3600) snprintf(head, sizeof head, "%ld 分钟前同步", age / 60);
    else snprintf(head, sizeof head, "%ld 小时前同步", age / 3600);
    if (lst.st_mtime != 0 && age > 1800) hc = C_YELLOW; /* 3 个抓取周期 */
    v_text_right(cr, v->lw - mx, L_TOP_Y + 13, FD_HEAD, hc, "%s · %s · Open-Meteo", head,
                 campus);
    v_rule(cr, mx, L_RULE1_Y, mw);

    if (ncity <= 0) {
        v_text_center(cr, v->lw / 2, v->lh / 2 - 14, v_fd_big(), C_OVERLAY0, "天气还没同步");
        v_text_center(cr, v->lw / 2, v->lh / 2 + 20, FD_LABEL, C_OVERLAY0,
                      "先跑一次 systemctl start hpopi5b-weather.service");
        v_text_center(cr, v->lw / 2, v->lh / 2 + 42, FD_LABEL, C_OVERLAY0, "缓存：%s",
                      cache_path);
        return;
    }

    /* ---- 主城 hero ---- */
    wx_city_t *m = &city[0];
    double hy = L_BODY_Y;
    v_text(cr, mx, hy, FD_LABEL, C_OVERLAY1, "%s", m->name[0] ? m->name : "佛山 · 南海 · 狮山");
    char tb[32];
    snprintf(tb, sizeof tb, "%.1f°", m->temp);
    v_text(cr, mx, hy + 12, v_fd_huge(), C_TEXT, "%s", tb);
    double tw = v_text_w(v_fd_huge(), tb);
    v_text(cr, mx + tw + 18, hy + 30, v_fd_value(), wmo_col(m->code), "%s", wmo_text(m->code));
    v_text(cr, mx + tw + 18, hy + 60, FD_LABEL, C_OVERLAY0, "今天 %.0f° ~ %.0f°", m->tmin, m->tmax);
    v_text(cr, mx, hy + 84, FD_LABEL, C_OVERLAY1,
           "体感 %.1f° · 湿度 %.0f%% · %s %d 级 · 阵风 %.0f km/h · 气压 %.0f hPa · 云量 %.0f%%",
           m->feels, m->rh, dir_word(m->wind_dir), beaufort(m->wind), m->gust, m->pressure,
           m->cloud);

    /* ---- 未来 24 小时曲线 ---- */
    double title_y = L_BODY_Y + L_WX_HERO_H;
    v_text(cr, mx, title_y, FD_LABEL, C_OVERLAY1, "未来 24 小时");
    double hi = -1e9, lo = 1e9;
    for (int i = 0; i < nh[0]; i++) {
        if (hour[0][i].temp < -100) continue;
        if (hour[0][i].temp < lo) lo = hour[0][i].temp;
        if (hour[0][i].temp > hi) hi = hour[0][i].temp;
    }
    if (lo <= hi)
        v_text_right(cr, mx + leftw, title_y, FD_LABEL, C_OVERLAY0, "最高 %.0f° · 最低 %.0f°", hi,
                     lo);

    double chart_y = title_y + L_WX_CHART_TITLE_H;
    if (nh[0] >= 2) draw_curve(cr, mx, chart_y, leftw, L_WX_CURVE_H, hour[0], nh[0]);
    else
        v_text(cr, mx, chart_y + 30, FD_LABEL, C_OVERLAY0, "24 小时逐时数据还没拿到");

    double precip_y = chart_y + L_WX_CURVE_H + 4;
    v_text(cr, mx, precip_y, FD_SMALL, C_OVERLAY0, "降水概率");
    if (nd[0] > 0 && day[0][0].precip_sum >= 0)
        v_text_right(cr, mx + leftw, precip_y, FD_SMALL, C_OVERLAY0, "今日降水 %.1f mm",
                     day[0][0].precip_sum);
    if (nh[0] >= 2) draw_precip(cr, mx, precip_y, leftw, L_WX_PRECIP_H, hour[0], nh[0]);

    /* 时间轴：每 6 小时一个标签 */
    if (nh[0] >= 2) {
        double step = leftw / (double)(nh[0] - 1);
        for (int i = 0; i < nh[0]; i++) {
            if (i != 0 && i != 6 && i != 12 && i != 18 && i != nh[0] - 1) continue;
            char lb[16];
            if (i == 0) snprintf(lb, sizeof lb, "现在");
            else snprintf(lb, sizeof lb, "%d时", hour[0][i].hour);
            double cx = mx + step * i;
            if (i == 0) v_text(cr, mx, precip_y + 16 + L_WX_PRECIP_H + 2, FD_SMALL, C_OVERLAY0, "%s", lb);
            else if (i == nh[0] - 1) v_text_right(cr, mx + leftw, precip_y + 16 + L_WX_PRECIP_H + 2, FD_SMALL, C_OVERLAY0, "%s", lb);
            else v_text_center(cr, cx, precip_y + 16 + L_WX_PRECIP_H + 2, FD_SMALL, C_OVERLAY0, "%s", lb);
        }
    }

    /* ---- 日出日落 + 空气质量 ---- */
    double foot_y = precip_y + 16 + L_WX_PRECIP_H + L_WX_AXIS_H + 8;
    if (nd[0] > 0) draw_sun(cr, mx, foot_y, m, &day[0][0], e);
    draw_aqi(cr, mx + 330, foot_y, m);

    /* ---- 右栏 7 天 ---- */
    struct tm tm;
    localtime_r(&e->wall, &tm);
    long today = days_from_civil(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    draw_days(cr, rightx, L_BODY_Y, rightw, today);

    /* ---- 底部关注城市 ---- */
    draw_city_strip(cr, mx, mw, band_top, today);
}

const scene_t SCENE_WEATHER = {"weather", "天气（广东东软学院 + 关注城市）", draw, sig, 0};
