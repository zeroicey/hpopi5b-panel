# Current task

<!-- The single live surface. Keep it under ~60 lines: it is read on every session start. -->

**Status:** 🟢 第十八轮（新闻幕三列化 + `scene_t.live`）已真机验收并部署，**待提交** · **Updated:** 2026-11-09 17:0x

## Goal

`hpcmd` 下线、全场 all-in-one 到 `hpcore`，面板**只画一台机**：主机区左右两个子列（各 2 张曲线，
1080p 下曲线高 135 逻辑像素）＋ 下方** Tailscale 尾网块**（两个子列各 5 台）＋ **一整列**进程榜
（按 CPU%，25 行，同值比 RSS ⇒ 空载时自动退化成内存榜）。
三幕（`panel` / `trending` / `news`）仍在，kiosk 默认 30s 轮换；桌面/锁屏模式不轮换。
新闻幕是 **1+2 列**（热点 13 条带量度杆 + 科技 28 条分两子列），**没有列标题行**，首条 92px 放大。

## Checklist

- [x] **第十三轮真机验收 + 部署**（0 warning、装二进制 + 探针、`systemctl restart`、取证数字见 Code state）
- [x] 提交 `17b8a4e` 并推送 `origin/main`
- [x] **第十四轮**：一条进程榜（25 行）+ 容器块 + 曲线 260→150 + 主机区 0.56→0.66；探针新增 `D` 行；提交 `001cc1e`
- [x] **第十五轮**：容器块整块**删除**，同位置换成 **Tailscale 尾网块**（探针读 tailscaled localapi，
      `T` 行 10 台；`D`/`docker.sock`/`ctr_*`/`draw_ctr_block`/`L_CTR_*` 全删）；真机验收并部署
- [x] **第十六轮**：趋势幕从 2×8 固定网格改**流式两列**（15 条全上屏 + 语言分布脚注）；
      挖出并修掉「行尾 `\n` 多算一行」与「每帧重排白烧 1.5% CPU」（排版计划缓存 `plan`）；
      文档：README / pitfalls §22/§23 / HANDOFF 第十六轮 / `.ai/pitfalls/cases/2026-11-06-trailing-newline-and-per-frame-layout.md`
- [x] 第十五/十六轮已提交（`001cc1e` / `5314146`）
- [x] **第十七轮**：新闻幕（左国内热点 14 条带热度 / 右科技 15 条带「来源 · 时间」）；
      `tools/fetch-news.py` + `hpopi5b-news.timer`（30 分钟）喂缓存 TSV，幕只读文件；
      选源实测表 + 版式规则写进 `docs/architecture.md` / `docs/design.md` 与
      `.ai/decisions/2026-11-08-news-scene-source-and-layout.md`；真机验收 + 部署 + 提交
- [x] **第十八轮**：新闻幕两列 → **三列 + 首条放大**（删列标题行、两行式 44px、量度杆左端钉死封顶 150、
      顶栏改报「上屏条数」）；挖出并修掉「1 Hz 采样快照无条件进全局签名 ⇒ 文件驱动的幕也每秒整屏重绘」
      （`scene_t.live`；真屏 **2.94% → 2.01%**）。落文档：`docs/design.md` 五·五 / `docs/pitfalls.md` §24 /
      `.ai/pitfalls/cases/2026-11-09-sig-mixes-live-metrics.md` / HANDOFF 第十八轮 / 决策修订小节
- [ ] 第十八轮 `git commit`（上轮 push 曾因 hpcore → github:443 TLS 握手失败而未推，先确认可达）
- [ ] `--rotate` 最终值仍没定（候选 25/30/45）
- [ ] `tools/deploy.sh` 支持 hpopi5b 落点（现在仍指向 hphelm 的 `/usr/local/bin`）
- [ ] CPU 预算收口（见 Blockers —— 数字口径已经变了，要先重定「预算怎么量」）
- [ ] 尾网块上限 10 台（`L_TS_ROWS 5 × 2`）；超过 10 台时标题的「N 台」会大于画出来的行数（现在 9 台，不冲突）
- [ ] 尾网没画出口节点（`ExitNodeStatus`）、没画 DERP 延迟（`netcheck` 很贵）、没画本机自身流量
- [ ] `tools/render-ascii.py` / `measure.py` 依赖 ImageMagick，hpopi5b 上没有 ⇒ 整屏 ASCII 与 `--bands` 仍不可用
      （已补 `tools/xshot.c` 截图 + `tools/pngcrop.py` 裁切/单行游程，够量分隔线与局部；整屏视图仍缺）

## Code state

`main` @ `5314146` + 第十七轮提交 · 第十七轮已部署到 hpopi5b（2026-11-08 22:0x）。
现场：`hpopi5b-kiosk` active · `pgrep -cx hpopi5b-panel` = 1 · **tty1** active ·
slick-greeter 0 · 无 X 错误 · `hpopi5b-news.timer` enabled（NEXT = 30 min 后）·
`/var/cache/hpopi5b-panel/news.tsv` 首行 `# news <时间> hot=14 tech=30`。
判据：`probe-frames 50 30` ⇒ **>30% 变化帧 0**；`LAYOUT_DEBUG=1` ⇒ `news: cw=617.6 y0=94.0 avail=609.7 rows=15`；
news 幕单帧绘制 ≈ **6 ms**（panel 28 ms）；真屏 CPU 3 幕轮换 **3%** vs 钉住 panel **2%**（同口径旧 2 幕 2.9~3.2%）。
**CPU A/B（真屏 :0，`performance` 模式 1.8 GHz，各 60s）**：旧三列 **3.47%** → 新单机 **3.68%**。
第十六轮（趋势幕流式两列）：旧 **3.47/3.10** → 新 **3.97/3.12**（+0.25pp 量级，噪声内）；
单帧绘制（趋势幕单幕、`HPOPI5B_PANEL_DEBUG=1` 的「绘制 Xms」中位）旧 **19.4ms** → 首版 29.0ms → 缓存后 **23.1ms**。
⚠️ 同一份二进制在 `ondemand` 空载（408 MHz）下量到 **5.8%** —— 频率让 CPU-秒差 5 倍，**只做 A/B 比**。
第十八轮（新闻幕三列化）：A/B 同口径 **旧 2.94% → 新 2.01%**（90s 窗口、3 幕轮换）；
单帧绘制 news ~20ms 但只在分钟翻转/缓存改写时发生；上屏 41 条（旧 29 条）。
第七轮到第十二轮的逐轮记录在 `.ai/sessions/2026-11-05-rounds-7-to-12-handoff.md`。

## Constraints

边界，不是阻塞项：决定改法，但没人在等它们。

- **CPU 预算 `<1% of one core`**：这个数在 hpopi5b 上量不出来（见 Blockers），当作「不许变慢」的相对门槛。
  再降的正解：PangoLayout 缓存（每秒 ~50 段文字）+ 「值没变就不重画那一行」的区域局部重绘。
- **两列固定高度必须相等**（`FIX_A == FIX_B == 187`）：「余」为负时禁止再压曲线（会挤字）。
- **曲线量程由 `v_chart_scale()` 统一算**，`v_spark/v_spark2` 必须显式传它返回的 vmax（网格与曲线不能各算一套）。
- **探针与面板同版本**：`P`/`Q` 4 段 + `T` 行；老探针仍兼容（CPU 列画 `-`，不冒充 0.0%），
  没有 `T` 行时尾网块整块消失且 `ts_h = 0`（曲线自己长回去），不留空洞。
- **尾网数据只走 tailscaled 的 localapi**（`/var/run/tailscale/tailscaled.sock`，权限 0666 ⇒ 不要 root），
  **不 fork `tailscale status`**（起进程 + 几百毫秒，1 Hz 探针扛不住）。
  ⚠️ JSON 里对端列表的键是 **`Peer`（单数）**，写 `Peers` 会静默得到 0 台。
- 容器只剩「个数」（cgroup 目录计数，不碰 socket）；**不要再做容器清单**（用户已否：容器里的进程本来就在进程榜里）。
- **新闻幕的缓存契约**：`H\t标题\t源\t热度` / `T\t标题\t源\tunix_ts`；抓不到保上一份，**不灌假数据**；
  热搜**没有分类字段**（不要做关键词过滤冒充分类）；榜单右对齐量化值统一 `C_YELLOW`（不借 `C_PEACH`）。
- **测试实例与真机实例同名** ⇒ 杀进程按 `/proc/<pid>/environ` 的 `DISPLAY` 挑 pid，永不 `pkill -f`。
- 换二进制先 `systemctl stop`，否则 `install`/`cp` 撞 `Text file busy`（本轮踩过）。
- **`scene_t.live`**：画面会随 1 Hz 采样变的幕写 1（目前只有 `panel`），只吃外部文件的写 0
  （`trending`/`news`）。写错不会画错，但会让那一幕每秒整屏重绘（≈1% of one core）。
- **行高/字高用 `v_text_h()` 量**（13pt 实测行高 24，不是 18），不要拿字号乘系数估版式。

## Blockers

- CPU 数字口径 — waiting on owner：`ondemand` 408 MHz 空载下 3.5~5.8% 到底算不算超预算？
  选项：① 改文档预算为「A/B 不变慢」；② 固定 `performance` 模式（费电、Pi 烫）；
  ③ 真做 PangoLayout 缓存 + 区域局部重绘再谈绝对值。

## Pitfall reminders for the current branch

- #15 绝不 `dm-tool` / `light-locker`（会把 lightdm greeter 招回来并抢走 VT）
- #16 `sleep_off=0` = 永不关屏；裸比较会让 0 变成「立即关屏」
- #17 **量 CPU 必须同时报 load average、窗口长度**，并且**同一频率下 A/B**（`ondemand` 会让同一份代码差 5 倍，
  见 `.ai/pitfalls/cases/2026-11-06-cpu-measure-freq-confound.md`）
- #20 **量性能前先 `ps -eo pcpu,pid,comm --sort=-pcpu | head`**：本轮发现 5 个孤儿 `/tmp/render_tr`
  各烧 100% CPU 已 3 天（load 5.2），任何 CPU 测量都会被它带偏
- #18 加宽/加列后必须重跑 `--bands` 量竖带起点（当前 ImageMagick 缺失，这条暂时只能靠代码算）
- #19 外部脚本读 `--dump` 一律按前缀 grep，不要依赖行序

## Layout invariants（改版面时必看）

- 主机区两子列**各自流式**：`L_H_*` 是各行高/间距，`fixed` 表必须与绘制顺序逐段对应
- 判据：`LAYOUT_DEBUG=1` ⇒ `FIX_A=187 FIX_B=187 余=0.0`、`ch ≤ L_H_CHART_MAX(150)`
- 纵向顺序：主机块（`bottom = 712 - ts_h`）→ 主机块自己的说明行（`L_H_CAP_H 16`）→ 尾网块
  （`ts_h = 18 + 24 + 5×22 + 16 = 168`）；没有尾网数据时 `ts_h = 0`
- 尾网行内列位从右往左算（速率 100 → 连接方式 62 → 系统 52 → 名字吃剩下的）；「系统」列必须**右对齐**，
  `windows` 比槽位宽，左对齐会压到「中继 hpnet」上（第一版就这么叠了）
- **判版面不要看缩放截图**：先 `LAYOUT_DEBUG` 算，再 `tools/pngcrop.py` 数像素（见 pitfall 2026-11-06）
- 圆点用 `v_text_right_ex()` 返回的墨迹中线画，**不要用常数偏移**
- 右对齐数值（load / 缓存 / 丢错 / 量程文字）一律先用 `v_text_w` 量宽再决定左边能放什么，禁止硬叠

## Next action

1. 把 Blockers 的「预算口径三选一」交给 owner。
2. 定 `--rotate`（现在 3 幕 ⇒ 90s 一圈）；`tools/deploy.sh` 改成支持 hpopi5b 落点。
3. 新闻幕的三个小尾巴：36氪 feed 解析 0 条；`--hot 14` < 一列 15 行；顶栏「热点 14 · 科技 30」是池子大小而非上屏条数。
4. 若 owner 要真降 CPU：先加 PangoLayout 缓存（`v_text` 里按 (字符串, 字号, 颜色) 缓存 layout），再量 A/B。
