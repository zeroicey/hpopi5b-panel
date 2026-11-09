# 文件驱动的幕也每秒整屏重绘：全局内容签名混进了 1 Hz 采样快照

**Severity:** 🟠 medium（不坏功能，纯粹烧 CPU：≈1% of one core）· **First hit:** 2026-11-09 · **Hits since:** 0

## Symptom

钉住新闻幕（`--scene news`）时，`HPOPI5B_PANEL_DEBUG=1` 每秒打一行 `重绘 1 · 绘制 18~25ms`，
而新闻缓存 `news.tsv` 30 分钟才更新一次。合计 ≈ **2% of one core**，占掉「<1% 预算」的两倍。

## Root cause

`src/panel.c` 的 `content_sig()` 无条件把三样**每秒都会变**的东西混进全局签名：

- `M` —— 本机 1 Hz 采样快照（为面板幕准备的）；
- `R.m` —— 远端快照（同上）；
- `FEED.age_s` —— 值是 `now - last_frame`，**在线时也每秒在涨**（离线才该有意义）。

`trending` / `news` 只吃外部 TSV，自己的 `sig()` 已经混了文件 `mtime` 与「整分钟」，
全局签名一抖就会整屏重画 —— 内容根本没变。

## Fix

1. `scene_t` 加 `int live`（1 = 画面随 1 Hz 采样变）：`panel=1`、`trending=0`、`news=0`。
2. `content_sig()` 只在 `state != ST_PANEL || sc->live || scenes_transitioning()` 时混 `M` / `R.m`。
3. `FEED.age_s` 只在**离线**时混：`age_s >= 0 && !FEED.online`（面板要持续走「离线 Ns」，那一段由
   `scene_panel.c` 自己的 sig 保证）。

## Guard

```sh
ssh hpopi5b 'cd /tmp/panelb && (DISPLAY=:9 HPOPI5B_PANEL_DEBUG=1 timeout 20 ./hpopi5b-panel \
  --kiosk --scale 1.48 --scene news --lock /tmp/x9.lock >/tmp/x9.log 2>&1 &) ; sleep 16
  grep -a "重绘" /tmp/x9.log | tail -3'
# 通过：重绘 0 / 绘制 0.0ms 连续多秒（分钟翻转那一刻可以有 1 次）
# 失败：每秒 重绘 1 / 绘制 ~20ms
```

新增一幕时先回答：**我的画面会自己变吗？** 不会 ⇒ `live = 0`，内容签名只靠 `sig()` 里的文件/分钟。

## Numbers

真屏 :0，`performance` 1.8 GHz，90 s 窗口，3 幕轮换：
**旧二进制 2.94% → 新 2.01%**（新幕内容还多了 40%）。

详见 `docs/pitfalls.md` 第 24 条。
