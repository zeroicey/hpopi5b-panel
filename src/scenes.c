/* scenes.c —— 幕的注册表 + 轮换调度（纯逻辑，不碰 X11）
 *
 * 轮换时间的口径（2026-11-05）：
 *   · **统一一个数** `--rotate S`（默认 30s），不搞每幕不同停留 —— 4 幕以内一个数最好解释，
 *     也最容易在真机上调。之后如果出现「信息密度差很多」的幕，再给 scene_t 加 dwell 字段。
 *   · 30s 的来历：密排面板（三列 + 进程榜）从抬头到看完约 8~12s，留 2~3 倍余量；
 *     少于 15s 会一直处在「还没看完就换」的焦虑里，而且转场本身要 0.55s。
 *   · 轮换总周期 = 幕数 × S：4 × 30 = 2 分钟。超过 2 分钟，值班的人会漏掉告警页；
 *     以后真要放很多幕，改成「告警幕常驻 + 其他幕轮换」，不要靠加快轮换解决。
 *   · 转场 0.55s：低于 0.3s 看着像闪烁，高于 1s 会让人觉得卡。
 */
#include <math.h>
#include <string.h>

#include "scene.h"

static const scene_t *SCENES[] = {&SCENE_PANEL, &SCENE_TRENDING, &SCENE_NEWS, &SCENE_WEATHER};
#define N ((int)(sizeof SCENES / sizeof SCENES[0]))

static int cur = 0;
static int pin_idx = -1;
static int rotating = 1;
static double rotate_s = 30.0;
static double since;
static int fx_fixed = -1;
static scene_trans_t TR;
static unsigned gen = 1;

int scenes_count(void) { return N; }

const scene_t *scene_at(int i) { return (i >= 0 && i < N) ? SCENES[i] : NULL; }

int scene_index(const char *id) {
    if (!id || !*id) return -1;
    for (int i = 0; i < N; i++)
        if (!strcmp(SCENES[i]->id, id)) return i;
    return -1;
}

const char *scene_fx_name(scene_fx_t fx) {
    switch (fx) {
    case FX_CUT: return "cut";
    case FX_DIP: return "dip";
    default: return "?";
    }
}

int scene_fx_by_name(const char *name) {
    if (!name || !*name) return -1;
    for (int i = 0; i < FX__N; i++)
        if (!strcmp(name, scene_fx_name((scene_fx_t)i))) return i;
    return -1;
}

void scenes_reset(double now, int pin, double rotate, int fx) {
    cur = (pin >= 0 && pin < N) ? pin : 0;
    pin_idx = cur;
    rotating = rotate > 0 && N > 1 && pin < 0;
    rotate_s = rotate > 0 ? rotate : 30.0;
    fx_fixed = (fx >= 0 && fx < FX__N) ? fx : FX_DIP;   /* 不指定就淡入淡出 */
    since = now;
    TR.active = 0;
    memset(&TR, 0, sizeof TR);
    gen++;
}

int scenes_tick(double now) {
    if (TR.active) {
        if (now >= TR.t0 + TR.dur) {
            for (int i = 0; i < N; i++)
                if (SCENES[i] == TR.to) cur = i;
            TR.active = 0;
            since = now;
            return 1;
        }
        return 1;
    }
    if (!rotating) return 0;
    if (now - since < rotate_s) return 0;

    int to = (cur + 1) % N;
    scene_fx_t fx = (scene_fx_t)fx_fixed;
    TR.active = 1;
    TR.from = SCENES[cur];
    TR.to = SCENES[to];
    TR.fx = fx;
    TR.t0 = now;
    TR.dur = (fx == FX_CUT) ? 0.0 : 0.55;
    TR.gen = ++gen;
    if (fx == FX_CUT) { /* 硬切：没有中间帧，直接把「当前」换掉 */
        TR.active = 0;
        cur = to;
        since = now;
    }
    return 1;
}

const scene_t *scenes_current(void) { return SCENES[cur]; }
int scenes_current_index(void) { return cur; }
scene_trans_t *scenes_transition(void) { return &TR; }
int scenes_transitioning(void) { return TR.active; }
int scenes_rotating(void) { return rotating; }

void scenes_resume(double now) {
    if (TR.active) {
        for (int i = 0; i < N; i++)
            if (SCENES[i] == TR.to) cur = i;
        TR.active = 0;
    }
    since = now;
}

double scenes_dwell_left(double now) {
    if (!rotating) return -1;
    if (TR.active) return 0;
    double left = rotate_s - (now - since);
    return left > 0 ? left : 0;
}