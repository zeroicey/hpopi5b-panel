# AGENTS.md —— 给「负责 hpopi5b-panel 的 AI」的工作指南

> 你是这个项目的专职维护者。这份文件是你的操作手册：**先读它，再动手**。
> 详细材料在 `docs/`；逐轮交接与当前待办在 `HANDOFF.md`；**项目记忆库在 `.ai/`**（见文末契约）。
> 最后更新：**2026-11-06**（hpcmd 下线 ⇒ 源码工作副本搬回 hpcore，面板单机化）。
> **现役形态**：源码工作副本 = **hpcore `/home/happpy/work/project/hpopi5b-panel`**（本机没有 gcc/make ⇒ 只改代码 + push，构建/部署在 hpopi5b 上做）；
> 运行机 = **hpopi5b**（Orange Pi 5，kiosk 纯显示终端）。下面 2026-10-21 的「本机原地部署 / lightdm / 锁屏」
> 章节属于 hphelm 时代，**已失效**；部署以 `docs/deploy.md` 的「三十秒版」为准。

---

## 0. 三十秒上手

```sh
cd /home/happpy/work/project/hpopi5b-panel   # hpcore（本机）：只有源码，没有 gcc/make

# 同步到 hpopi5b 编译 + 离屏验证（本机编不了；Xvfb/截图都在 hpopi5b 上）
tar --exclude=.git --exclude='*.o' -czf /tmp/s.tgz . && scp -q /tmp/s.tgz hpopi5b:/tmp/
ssh hpopi5b 'cd /tmp/panelb && tar -xzf /tmp/s.tgz && make panel'
ssh hpopi5b 'cd /tmp/panelb && DISPLAY=:9 ./hpopi5b-panel --kiosk --scale 1.48 \
  --remote hpcore --scene panel --lock /tmp/x99.lock'     # 先起 Xvfb :9，别动真屏

# 装到真屏（判据见 docs/deploy.md「三十秒版」）
ssh hpopi5b 'install -m755 /tmp/panelb/hpopi5b-panel /usr/local/lib/hpopi5b-panel/hpopi5b-panel \
  && install -m644 /tmp/panelb/tools/probe-remote.pl /usr/local/share/hpopi5b-panel/probe-remote.pl \
  && systemctl restart hpopi5b-kiosk'
```

⚠️ `tools/deploy.sh` 是 hphelm 时代「本机原地部署」，**在 hpcore / hpopi5b 上都不要跑**。

⚠️ **`tools/deploy.sh` 必须在本机跑**：2026-10-19 起源码与部署目录合一，没有 rsync、不 ssh 自己。
旧的 `ssh hphelm 'make …'` 写法**必然失败**（本机 `~/.ssh/config` 没有 `hphelm` 这个 Host）。
部署的完整说明（七个阶段、判据表、三层回滚、六件绝不能做的事）在 **[`docs/deploy.md`](docs/deploy.md)**。

**这个项目是什么**：`hphelm`（机队里常驻工位的那台）的**运维看板 + 自研锁屏**。
X11 + cairo + pango 纯自绘（无 GUI 工具包），约 5.7k 行 C。
行为：空闲→资源面板，动键鼠→登录页，PAM 认证→桌面，再空闲→面板（循环）。

**它不是什么**：不是通用面板、不是产品、不需要「适配更多环境」。
README 的「这不是给谁用的」那张表就是**设计边界**——不要为了通用性牺牲机队约束。

### 接手第一小时读什么（别一上来读 2000 行 C）

| 顺序 | 读什么 | 你会得到 |
| --- | --- | --- |
| 1 | 本文件（你正在读） | 红线、硬约束、工作流、验收判据 |
| 2 | `.ai/CURRENT_TASK.md` | 此刻在做什么、卡在哪、下一步是什么 |
| 3 | **`docs/deploy.md`** | **怎么改完就上真屏（含回滚与六件绝不能做的事）** |
| 4 | `HANDOFF.md` 前列 + 第四节 | 逐轮干了什么、已知粗糙点（下一轮候选） |
| 5 | `docs/architecture.md` · `docs/design.md` | 状态机/双缓冲/采样/认证 · 无卡片版式规则 |
| 6 | `docs/verify.md` · `docs/pitfalls.md` | 看不到图怎么验 · 16 条已踩的坑（每条带判据） |
| 7 | `.ai/` | 决策记录（`.ai/decisions/`）、坑案例、上一轮 handoff |

---

## 1. ⛔ 三条红线（**碰了就会让用户看到不该看到的东西**，我们为此踩过坑）

### 红线 1：永远不要向 lightdm 发锁请求

面板是 `override-redirect` 全屏覆盖层，**只在活跃 VT 上可见**。只要有人把锁请求发给 lightdm
（`dm-tool lock` / `light-locker` / 让 autologin 失败），lightdm 就会**起 greeter 并把 VT 切到 tty8**
⇒ 用户看到的是 DM 的旧登录页，自研面板被藏在后面。

⇒ **`hphelm-session` 里永远不出现 `dm-tool`**；守护循环的兜底**只允许 `xset dpms force off` + 记日志**。

### 红线 2：守护循环只把「运行 < 5 秒即退出」当失败

`pkill` 默认 SIGTERM ⇒ 退出码 **143**。用「非 0 即失败」判定的话，**每次调试重启都会给失败计数 +1**，
攒到 3 次触发兜底（历史上就是这么把 greeter 招回来的）。**判据用运行时长，不要用退出码。**

### 红线 3：回滚时先停 VT 守卫

`hphelm-vt-guard.service`（独立防线：活跃 VT≥8 且 slick-greeter 在跑且 bspwm 在跑 ⇒ `chvt 7`）
会把 VT 一直往 tty7 拽。**回滚/改会话入口前先 `sudo systemctl disable --now hphelm-vt-guard.service`。**

> 三条的完整坑因、判据、以及「为什么兜底不能交给 DM」见 [`docs/pitfalls.md`](docs/pitfalls.md)
> 与 `kb` 的 `pitfalls/2026-10-18-guard-fallback-self-inflicted-traps.md`。

---

## 2. 硬约束（**不可协商**，改之前先想清楚）

| 约束 | 数字 | 为什么 |
| --- | --- | --- |
| **不许引 GUI 工具包** | GTK4 = 119 MiB vs 自绘 7.4 MiB | 这是本项目的**立项理由**（实测见 README 表） |
| CPU 预算 | 目标 **< 1% of one core**（现状 **~1.0% 只本机 / ~1.4% 含生产服务器**，略超；见 `HANDOFF.md` 三·八） | 它 7×24 常驻在工位机上 |
| 内存预算 | Private/Anon **约 5 MiB**（PSS 里大部分是共享库页） | 同上 |
| 线程数 | 2 | 采样在主循环里同步做，不要引入线程池 |
| 采样频率 | **1 Hz**（`metrics_sample` 按时间节流） | 不要「每轮循环采样一次」——循环速率一变数值就变噪声 |
| 渲染 | **离屏 pixmap + 每帧一次 `XCopyArea`**；背景渐变另存一张 `bgpix` 只渲染一次 | 直接画窗口会闪（见 `docs/architecture.md`） |

性能是**验收项**，不是「有空再优化」：每轮改完都要报 CPU 与内容变化率。

---

## 3. 工作流（照这个顺序做，别跳）

```
① 读 HANDOFF.md 第四节（当前候选） + docs/design.md（如果要动 UI）
② 在 Xvfb 里改到满意（--lock 隔离，别动真机）
③ 用 docs/verify.md 的方法量像素 / 截图取证（`tools/xshot.c` 是本轮新写的纯 X11 截图器）
④ 真机：scp 到 hpopi5b → `make panel` → `install` 二进制 + 探针 → `systemctl restart hpopi5b-kiosk`
   （见第 0 节与 `docs/deploy.md`「三十秒版」；屏幕抖一下是正常的）
⑤ 真机判据全过（见第 4 节）
⑥ 文档：HANDOFF.md 记本轮 + README/docs 同步 + git commit
⑦ 机队事实有变（部署位置/服务名/红线）⇒ 提醒维护 kb 的人更新 `kb/runbooks/hphelm.md`
```

**改 UI 的第一原则：先量后改。** 不要凭想象改版式——用 `tools/render-ascii.py` 看整屏、
`tools/render-region.py` 看局部、`tools/measure.py` 量几何（柱宽/间距/对齐/行距）。

⚠️ 这三个 python 工具都依赖 **ImageMagick（`magick`）**，`hpopi5b` 上没有装 ⇒ 当前链路上**用不了**。
替代：`tools/xshot.c`（`cc -O2 -o xshot tools/xshot.c $(pkg-config --cflags --libs x11 cairo)`）
把窗口直接拷成 PNG，再靠 `LAYOUT_DEBUG=1` 打出的 `wa/ch/FIX_*/余` 与代码里的常量对账。

---

## 4. 验收判据（**每轮都必须跑，缺一项不算完**）

（`tools/deploy.sh` 是 hphelm 时代的本机原地部署，**在这里跑不了**；下面逐项跑。）

```sh
# ① 不闪：抓窗口连续帧，>30% 像素变化的帧必须是 0（kiosk 每 30s 换幕那 2 帧除外）
DISPLAY=:0 tools/probe-frames 60
# ② 主循环正常：2 it/s、poll 全是超时、事件 0
HPOPI5B_PANEL_DEBUG=1 ./hpopi5b-panel --state panel --lock /tmp/x.lock   # 每秒一行
# ③ CPU：读 /proc/<pid>/stat 的 utime+stime（jiffies × 10ms / 窗口 ms）
#    ⛔ 只做 A/B（旧二进制留在 …/hpopi5b-panel.prev），并先固定频率：
#    echo performance > /sys/devices/system/cpu/cpufreq/policy0/scaling_governor
#    （ondemand 空载 408MHz，同一份代码量出 5.8%；1.8GHz 下 3.7%。详见 docs/verify.md 五节）
# ④ 红线没被碰：VT 还在 tty1、没有 greeter、单实例
cat /sys/class/tty/tty0/active          # 期望 tty1（hpopi5b 的 xinit 就是 Xorg :0 vt1；hphelm 时代是 tty7）
pgrep -cx slick-greeter                 # 期望 0
pgrep -cx hpopi5b-panel                  # 期望 1
# ⑤ 几何（改 UI 时）：量柱宽/间距/对齐/行距，与设计值逐位对
python3 tools/measure.py assets/panel.png --runs 240 --bands
```

⚠️ **单位别写错**：`CLK_TCK=100` ⇒ 1 jiffie = 10ms。CPU 百分比一律由
「毫秒 / 窗口毫秒 × 100」现算（我在这里错过 10 倍，见 `docs/verify.md` 末尾）。

---

## 5. 设计系统（改 UI 必读 `docs/design.md`）

- **不用卡片**：分区只靠「1px 细线 + 字号层级 + 留白」。没有圆角框、没有底色块。
- **紧凑**：区块标题不单独占行（label 与值同排、右对齐）；坐标只写在 `src/theme.h` 的 `L_*` 常量里。
- **逻辑像素**：所有绘制在逻辑像素下进行，主循环统一 `cairo_scale(SCALE)`；不要硬编码物理像素。
- **曲线**：量程用 `v_nice_ceil()` 吸附到 1/2/5×10ⁿ；CPU/内存固定 0~100。
  ⛔ **不要用「当前值」预填曲线**（等于伪造历史，还会把量程顶上去压扁真数据）——
  空态用「零基线 + 诚实生长」表达。
- **配色**：白天 Latte / 夜间 Mocha，**只走 `theme.h` 的语义宏**（别在 `view_*.c` 里写死色值，否则该幕不会跟随主题）；语义色别乱用（mauve=CPU/主色、blue=内存、green=网络/正常、
  yellow=进程/警告、red=危险、teal/peach=下行/上行）。

---

## 6. 代码地图

| 文件 | 作用 | 改它的频率 |
| --- | --- | --- |
| `src/theme.h` | 语义色宏 + 版式常量（逻辑像素）+ 字号 | ⭐ 最常动 |
| `src/theme.c` | 白天/夜间两套调色板 + 固定时刻判定 | ⭐ 加主题只动这里 |
| `src/view_panel.c` | 面板版面（主机区两子列 + 尾网块 + **一整列**进程榜） | ⭐⭐ 改面板只动这里 |
| `src/scene.h` · `src/scenes.c` | 幕的接口（draw + 自身内容签名）与轮换/转场调度 | ⭐ 改调度只动这里 |
| `src/scene_*.c` | 各幕：panel / trending / news / weather | ⭐⭐ 加幕就新建一个文件 |
| `src/view_login.c` | 登录页版面 | ⭐ 改登录页时 |
| `src/view.c` | 绘制基元（文字/渐变/细线/条/每核柱/sparkline/量程） | 偶尔 |
| `src/metrics.c` | **本机**数据采集（`/proc`、`/sys`，不 fork） | 加指标时 |
| `src/remote.[ch]` | **生产服务器**采集（ssh 长连接 + 非阻塞解析 + 退避重连） | 改远端协议时 |
| `tools/probe-remote.pl` | 喂给被监控机的**只读**探针（从 stdin 进，不落盘到对方）；`P`=按内存榜（仅调试）、`Q`=按 CPU 榜 40 条、`T`=尾网 10 台（tailscaled localapi：直连/DERP、在线、瞬时速率） | 加远端指标时 |
| `tools/xshot.c` | 纯 X11 窗口截图 → PNG（零依赖；hpopi5b 上没有 magick 时的取证手段） | 偶尔 |
| `tools/pngcrop.py` | 纯标准库 PNG 裁切/放大/单行游程（补 magick 缺席，量像素用） | 偶尔 |
| `src/panel.c` | X11 窗口、事件、状态机、主循环、双缓冲、幕渲染入口、仪表 | 改交互时 |
| `src/auth.c` + `src/hpopi5b-auth.c` | 认证（助手是唯一的特权代码，~120 行） | 一般不动 |

---

## 7. 不要做什么

- ❌ 引 GUI 工具包（GTK/Qt/FLTK），或为了省事用 Python 重写（+19.5 MiB）。
- ❌ 直接往可见窗口上画（会闪）——所有绘制必须进离屏 pixmap。
- ❌ 忘记 `gcv.graphics_exposures = False`（会让主循环自激空转，CPU 飙 50~80 倍）。
- ❌ 用「每轮循环采样一次」代替时间节流；用 `difftime` 的整秒差算速率。
- ❌ 向 lightdm 发任何锁请求；用退出码判定守护循环失败；回滚不先停 VT 守卫。
- ❌ 用 `pkill -f <pattern>`（会匹配到执行它的 shell 自己，我们踩过**五次**）——用 `pkill -x` 或写进脚本；
  测试实例与真机实例同名，要按 `/proc/<pid>/environ` 里的 `DISPLAY` 挑 pid。
- ❌ 把版式坐标硬编码在 `view_*.c` 里（常量归 `theme.h`）；也**不要**把绝对坐标当相对偏移用
  （本轮就这么错了一次，整列下移 100px、编译零告警）。
- ❌ 在机器忙的时候量 CPU（`uptime` 的 load > nproc/2 就别量）——量到的是别人的负载。
- ❌ 把 `metrics_t` 之类的大结构放栈上（`read_procs` 的 `all[512]` 是 `static`）。
- ❌ 为了「看起来不空」灌假数据。
- ❌ 把项目假设包装成「通用产品」；也不要把「我们机队的口径」当成通用概念。

---

## 8. 交付纪律

1. **每轮结束**：`HANDOFF.md` 追加本轮小结（做了什么 / 判据数字 / 踩了什么）+ 更新「待做」。
2. **改了行为/接口/版式** ⇒ 同步 `README.md` 与对应的 `docs/*.md`（README 是门面，docs 是细节）。
3. **提交信息**：一句话说清「改了什么 + 为什么」，带判据数字（沿用现有提交风格，中文即可）。
4. **机队事实变更**（部署路径、服务名、红线、VT 守卫）⇒ 必须提醒维护 kb 的人更新
   `kb/runbooks/hphelm.md` 的「运维面板（hpopi5b-panel）」节（kb 主副本在 `hphelm:/data/kb`）。
   **代码知识归本仓库，机队事实归 kb。**
5. **不要 force-push、不要改历史**；`main` 分支直接提交（本项目单人维护）。

---

## 9. 环境速查

| 项 | 值 |
| --- | --- |
| 源码（权威） | **`hpcore:/home/happpy/work/project/hpopi5b-panel`**（本机；2026-11-06 hpcmd 下线后搬回） |
| 构建 + 运行机 | **`hpopi5b`**（Orange Pi 5，arm64 kiosk，纯显示终端）· 构建目录 `/tmp/panelb`（离屏）· 部署目录 `/usr/local/lib/hpopi5b-panel` |
| 本机 | `hpcore`（x86 all-in-one：docker + 各种服务）· Arch · **无 gcc/make/pkg-config/Xvfb** |
| 公开镜像 | `github.com/zeroicey/hpopi5b-panel`（`origin`，public，无 license） |
| 推代码 | ✅ `hpcore` 上有 `gh` 登录态（账号 `zeroicey`，https）⇒ 提交与 `git push` 在本机做即可（hphelm/hpcmd 时代的「推不上去」已不适用） |
| 二进制 | hpopi5b：`/usr/local/lib/hpopi5b-panel/hpopi5b-panel` + `.../hpopi5b-auth`（4755 root:root）· 探针 `.../share/hpopi5b-panel/probe-remote.pl` |
| 上一版二进制 | `/usr/local/bin/hpopi5b-panel.prev`（`deploy.sh` 内容不同时自动留） |
| 会话入口 | `/usr/local/bin/hphelm-session`（守护循环 + 日志轮转） |
| 运行日志 | `/tmp/hpopi5b-panel.log`（会话启动时按大小轮转） |
| PAM 服务 | `/etc/pam.d/hpopi5b-auth`（**专用**，只留 `pam_unix`） |
| 机队事实权威 | `kb` 的 `runbooks/hpopi5b-panel.md`（本机副本 `/data/kb`，主副本在 hphelm；旧内容散在 `runbooks/hphelm.md` 的「运维面板」节） |
| 项目记忆库 | `.ai/`（engram 初始化于 2026-10-21，现用 0.6.x：`npx -y @zeroicey/engram@0.6`）· `engram dump` 出 ≤1.5 KB 指纹 · `engram sync --check` 是 CI 闸 |

<!-- engram:managed -->

<!-- engram:contract:start v1 -->
## Project memory bank (`.ai/`) — required contract

This repository keeps a portable, tool-independent memory bank in `.ai/`. It outranks chat
history and your own recollection. Read it on demand with file tools; never paste it wholesale.

### Read before you act

1. `.ai/CURRENT_TASK.md` — the current goal, in-progress state and blockers. Read this first.
2. Newest-first scan of `.ai/decisions/` for `💭 PROPOSAL` / `✅ ACCEPTED` entries that touch
   your task. An accepted decision is binding; a proposal needs a verdict, not silent adoption.

### Read on demand (trigger → file, not a bulk preload)

| Trigger | Read |
| --- | --- |
| changing architecture, stack, module boundaries | `.ai/ARCHITECTURE.md` |
| deploy, env vars, CI, incidents, ops | `.ai/runbooks/<topic>.md` |
| a bug that feels familiar / "we fixed this before" | `.ai/pitfalls/cases/<case>.md` |
| resuming after context loss, compaction or a long session | newest `.ai/sessions/*-handoff.md` |
| brainstorming a direction, before writing code | `.ai/decisions/` (write a `💭 PROPOSAL`) |

### Write back (not optional)

| Event | Write |
| --- | --- |
| a choice that is expensive to reverse | `.ai/decisions/YYYY-MM-DD-<topic>.md` via `/remember-decision` |
| a pitfall you hit, or a silent failure mode you decoded | `.ai/pitfalls/cases/<case>.md` via `/remember-pitfall` |
| ending a session with unfinished or fragile work | `.ai/sessions/YYYY-MM-DD-<topic>-handoff.md` via `/handoff` |
| any change to goal, state or blockers | `.ai/CURRENT_TASK.md` (same edit turn, never "later") |

Rules of the bank:

- One file per topic, `YYYY-MM-DD-<kebab-topic>.md`, status header on line 1 of the body:
  `💭 PROPOSAL` → `✅ ACCEPTED` → `🪦 REJECTED`. Flip the status, never rewrite history.
- Keep entries short and factual. The bank is an index of decisions, not a diary.
- Canonical skill specs live in `.ai/skills/*.md`. If your tool can load them, load them; if
  it cannot, this table is the fallback contract — follow it literally.
- If the bank and the code disagree, the bank is stale: fix the bank in the same change.
<!-- engram:contract:end -->
