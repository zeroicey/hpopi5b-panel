# 坑索引（本文件的权威在 `docs/pitfalls.md`）

`.ai/pitfalls/cases/` 只放**新**的、代码级或部署级的解码案例；历史 16 条坑的完整正文在
[`../../docs/pitfalls.md`](../../docs/pitfalls.md)（每条都带判据）。别在这里复制第二份。

## 必读的四条（碰了就会让用户看到不该看到的东西）

| # | 坑 | 一句话判据 |
| --- | --- | --- |
| 13 | 认证复用 `login` PAM 栈 | 输错 3 次 ⇒ 账号被 faillock 锁 ⇒ autologin 失败 ⇒ VT 被 greeter 抢 |
| 14 | 守护/兜底逻辑的三个自伤 | 失败判据用**运行时长**；兜底只许 `xset dpms force off` |
| 15 | 绝不用 `light-locker` / `dm-tool lock` | 两把锁 ⇒ 登录两遍；`dm-tool lock` 缺 `XDG_SEAT_PATH` 会静默失败 |
| 16 | `sleep_off = 0` 曾被写成「立即关屏」 | 裸比较 `idle >= 0` 恒真 ⇒ 判据必须 `CFG_SLEEP_OFF > 0 && …` |

## 本目录的案例

| 文件 | 一句话 |
| --- | --- |
| `2026-10-21-deploy-script-false-alarms.md` | 部署脚本三个假告警：`pgrep -q` 不存在 / 日志全文件 grep / CPU 窗口含启动秒 |
| `2026-10-21-benchmarking-cpu-under-foreign-load.md` | 在 load 23 的机器上量面板 CPU，数字全部作废；A/B 必须同窗口交替 |
| `2026-10-21-remote-feed-hangs-without-frames.md` | 「ssh 连上了但没数据」永不重连 ⇒ 面板永久显示离线；且运行态零日志 |
| `2026-10-21-three-column-layout-silent-misrenders.md` | 四个静默错版：相对/绝对坐标混用 · -1 兼职两种语义 · UTF-8 截断切半 · 固定高度表多算一块 |
| `2026-11-05-offscreen-harness-all-black.md` | 离屏 harness 出图全黑：忘了 `theme_set()` ⇒ `TP` 全 0（黑底画黑字）；用红矩形做对照排除导出链路 |
| `2026-11-06-cpu-measure-freq-confound.md` | `ondemand` 空载 408MHz 下同一份代码量出 5.8%，`performance` 固定 1.8GHz 只有 3.7% ⇒ CPU 结论必须 A/B + 固定频率 |
| `2026-11-06-tailscale-localapi-silent-zeros.md` | tailscaled 的 JSON 里对端 map 的键是 `Peer`（单数）；写成 `Peers` 只得到 0 台，HTTP 200 且无任何报错 |
| `2026-11-06-eyeballing-scaled-screenshots.md` | 用缩放截图的像素坐标判版面，三次把「正常」误读成「越界」；先算 `LAYOUT_DEBUG` 再 `pngcrop` 数像素 |
| `2026-11-05-probe-frames-vs-scene-rotation.md` | 判据「>30% 帧必须 0」在 kiosk 30s 换幕下必然假报警；换幕只满足「闪」三条指纹的第①条，用 `--rotate 0` 拿干净数 |
| `2026-11-06-trailing-newline-and-per-frame-layout.md` | 行尾 `\n` 没去掉 ⇒ pango 多算一行（`H_DESC=48` 而非 24），排不满却看不出来；修好后每帧重排又白烧 +1.5% CPU ⇒ 排版计划缓存 |
| `2026-11-09-sig-mixes-live-metrics.md` | 全局内容签名混进 1 Hz 采样 ⇒ 文件驱动的新闻幕也每秒整屏重绘（≈1% 单核）；`scene_t.live` 区分 |
| `2026-11-09-open-meteo-geocoding-ambiguity.md` | 中文小镇名动态 geocode 会命中河南/朝鲜同名地；天气坐标必须写死人核过的经纬度 |
