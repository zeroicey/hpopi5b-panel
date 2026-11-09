/* scene.h —— 可插拔「幕」的接口 + 轮换调度
 *
 * 为什么要这一层（2026-11-05 用户口径）：
 *   「单独搞一块开发板+显示器只显示资源面板有点浪费，它应该能显示点别的」
 *   ⇒ 面板不再是「唯一的一屏」，而是一个 registry 里的 **scene**；
 *     加一屏 = 加一个 src/scene_*.c 文件 + 在 scenes.c 的表里加一行，
 *     panel.c（X11/状态机/采样/双缓冲）完全不用动。
 *
 * 约定：
 *   · draw() 拿到的是**已经 cairo_scale 过**的上下文，坐标一律逻辑像素；
 *     背景由主循环预填（bg_done=1），scene 自己不要再画整屏渐变。
 *   · sig() 是内容签名：返回 0 表示「静态幕」（内容不会变，不重绘）。
 *     时间类幕把「整秒」揉进签名即可得到 1 Hz 刷新，不要自己开定时器。
 *   · 幕里不许做阻塞 I/O / 不许 fork / 不许自建线程（采集由主循环负责）。
 */
#ifndef SCENE_H
#define SCENE_H

#include <cairo/cairo.h>
#include <time.h>

#include "metrics.h"
#include "remote.h"
#include "view.h"

/* 一帧的数据环境（由 panel.c 每帧填好，幕只读） */
typedef struct {
    double now;          /* CLOCK_MONOTONIC 秒 */
    time_t wall;         /* 墙上时间（Pi 无 RTC，可能没有 NTP 同步） */
    metrics_t *m;        /* 本机快照 */
    remote_t *r;         /* --remote（可为 NULL 或 enabled=0；未启用时面板显示本机） */
    view_feed_t *feed;   /* 与 r 对应：扫描健康度 */
} scene_env_t;

typedef struct scene_s {
    const char *id;      /* --scene 用的标识（ASCII、kebab） */
    const char *label;   /* 日志/列表里的人话名字 */
    void (*draw)(cairo_t *cr, const view_ctx_t *v, const scene_env_t *e);
    unsigned long long (*sig)(const scene_env_t *e); /* 可为 NULL（= 静态） */
    int live;            /* 1 = 画面会随 1Hz 采样变（本机/远端指标）
                          * ⇒ panel.c 把 metrics 快照混进全局签名。
                          * 0 = 只吃外部文件/时间（trending、news）：
                          * 别让每秒都在涨的采样值把整屏重绘逼出来（实测 news 幕
                          * 一次绘制 ~20ms，每秒白烧半核的两个点）。 */
} scene_t;

/* ---- registry ---- */
int scenes_count(void);
const scene_t *scene_at(int i);
int scene_index(const char *id); /* -1 = 没有 */

/* ---- 转场 ---- */
typedef enum {
    FX_CUT = 0,   /* 硬切 */
    FX_DIP,       /* 淡出到底色再淡入（唯一默认转场：纯色覆盖，不需要 ARGB 合成） */
    FX__N
} scene_fx_t;

typedef struct {
    int active;
    const scene_t *from, *to;
    scene_fx_t fx;
    double t0, dur;
    unsigned gen;   /* 每次开始转场 +1：panel.c 用它缓存「两帧已渲染」 */
} scene_trans_t;

const char *scene_fx_name(scene_fx_t fx);
int scene_fx_by_name(const char *name); /* -1 = 不认识 */

/* ---- 调度 ----
 * pin >= 0：锁定该幕，永不轮换（--scene）
 * rotate <= 0：不轮换（--rotate 0）
 * fx = -1：每次换幕用下一种特效（默认）；否则固定特效
 */
void scenes_reset(double now, int pin, double rotate, int fx);
/* 返回 1 = 需要画一帧（开始转场 / 转场中 / 刚结束） */
int scenes_tick(double now);

const scene_t *scenes_current(void);
int scenes_current_index(void);
scene_trans_t *scenes_transition(void);
int scenes_transitioning(void);
double scenes_dwell_left(double now);
/* 回到面板时调用：打断进行中的转场、把停留计时从现在重新开始 */
void scenes_resume(double now); /* 当前幕还剩几秒（未轮换时 -1） */
int scenes_rotating(void);

/* 各幕（定义在 scene_*.c；scenes.c 只负责把它们排成一张表） */
extern const scene_t SCENE_PANEL;
extern const scene_t SCENE_TRENDING;
extern const scene_t SCENE_NEWS;

/* trending 幕的缓存文件（默认 /var/cache/hpopi5b-panel/trending.tsv） */
void scene_trending_file(const char *path);
/* 新闻幕的缓存文件（默认 /var/cache/hpopi5b-panel/news.tsv） */
void scene_news_file(const char *path);

#endif