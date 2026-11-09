/* scene_panel.c —— 「运维面板」幕：把单机版面包进 scene 接口
 *
 * 这里只做两件事：把数据环境转给 view_panel_draw()；给出内容签名。
 * 版面本身仍归 src/view_panel.c（改面板只动那里，别在这里加坐标）。
 *
 * 2026-11-06 单机化：hpcmd 下线、服务 all-in-one 到 hpcore ⇒ 面板只画一台主机。
 * 配了 --remote 就画那台（kiosk 上的 hpcore），没配就画本机（离屏调试用）。
 */
#include <stddef.h>

#include "scene.h"

static unsigned long long mix_bytes(unsigned long long h, const void *p, size_t n) {
    const unsigned char *b = p;
    for (size_t i = 0; i < n; i++) {
        h ^= b[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static unsigned long long sig(const scene_env_t *e) {
    unsigned long long h = 1469598103934665603ULL;
    /* 只把**正在显示的那台**算进签名：本机指标变了但屏幕上是 hpcore ⇒ 不该重绘 */
    if (e->r && e->r->enabled) h = mix_bytes(h, &e->r->m, sizeof e->r->m);
    else h = mix_bytes(h, e->m, sizeof *e->m);
#define MIX(v) do { h ^= (unsigned long long)(v); h *= 1099511628211ULL; } while (0)
    if (e->feed) {
        MIX(e->feed->online);
        MIX(e->feed->age_s >= 0 ? (long)e->feed->age_s : -1L);
    }
#undef MIX
    return h;
}

static void draw(cairo_t *cr, const view_ctx_t *v, const scene_env_t *e) {
    int remote_on = e->r && e->r->enabled;
    view_feed_t none = {0};
    view_panel_draw(cr, v, remote_on ? &e->r->m : e->m, remote_on ? e->feed : &none,
                    remote_on ? e->r->target : NULL);
}

const scene_t SCENE_PANEL = {"panel", "运维面板", draw, sig, 1};
