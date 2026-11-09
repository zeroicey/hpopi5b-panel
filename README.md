# hpopi5b-panel

> **现役形态（2026-11-03 起；2026-11-05 改名 `hphelm-panel` → `hpopi5b-panel`）**：跑在
> **hpopi5b（Orange Pi 5 + DP 显示器 1920×1080）**上的**纯显示终端** —— `--kiosk`，无登录页、
> 无桌面、不接键鼠；三幕（`panel` 运维看板 / `trending` GitHub 趋势 / `news` 新闻）按 `--rotate` 轮换。
> 源码工作副本在 **hpcore `/home/happpy/work/project/hpopi5b-panel`**（`hpcmd` 已于 2026-11-06 下线），
> 构建与部署在 `hpopi5b` 上做，见 [`docs/deploy.md`](docs/deploy.md)。
> **历史形态（已随 `hphelm` 2026-10-27 重装而消失）**：下文「锁屏 / 登录页 / PAM 认证」描述的是
> 工位机 `hphelm` 上的看板+自研锁屏；认证助手仍保留源码（`src/hpopi5b-auth.c`），但**生产不安装**。
>
> **English TL;DR** — A hand-drawn (no GUI toolkit) X11 **ops dashboard kiosk** for one display
> terminal in my home fleet (`hpopi5b`, Orange Pi 5): ~3.0k lines of C over cairo + pango,
> **~3.7% of one core** (measured A/B at a pinned 1.8 GHz; the Pi idles at 408 MHz, where the same
> binary reads 5.8%), ~20 MiB RSS, 1 thread, zero GUI toolkit dependencies. **One** host — the
> all-in-one `hpcore` — pulled over a long-lived ssh pipe with a read-only probe (nothing installed
> on the remote), plus its 24 busiest processes.
> Auto-rotating scenes (`panel` / `trending` / `news`); no login page in production.
> ⚠️ **This is not a drop-in product.** It is *my* answer for *my* machine, wired into our fleet's
> desktop stack and operational rules. Read [「这不是给谁用的」](#这不是给谁用的我为什么这么写)
> before copying anything.

![运维面板](assets/panel.png)

*（真实截图，非渲染图。上面是空闲时的资源面板，下面是同一套自绘风格的登录页。）*

![登录页](assets/login.png)

---

## 它做什么

把 `hphelm`（我机队里那台常驻工位的机器）的显示屏当成 **运维看板 + 锁屏**：不操作时显示资源面板，
动一下键鼠进登录页，认证通过回到桌面，再次空闲又回面板 —— **像屏保一样循环**。
登录界面**不用 lightdm 的 greeter**，由本项目自己画、自己认证（setuid 助手 + PAM）。

```
DESKTOP ──idle≥panel_idle──▶ PANEL ──input(≥input_grace)──▶ LOGIN
   ▲                                                          │
   └──────────────────── 认证成功（PAM）──────────────────────┤
LOGIN ──idle≥login_back──▶ PANEL（密码框留空、清错误）
PANEL ──idle≥sleep_off──▶ DPMS 关屏；任意输入唤醒
       └─ sleep_off ≤ 0 ⇒ **永不关屏**（默认，2026-10-19 起）
```

面板**只画一台机**：`hpcore（all-in-one）│ 进程榜`（用户口径 2026-11-06：「hpcmd 下线了，
全显示 hpcore 相关东西，把空间占满，进程那一列可以全部给他了」）。

- **主机区（左侧，约 66% 宽）是两个子列**，同一个 `draw_host()` 画，**每块只占一行**（标题嵌在数值里）：
  - 左子列：CPU（使用率 · 温度 · 频率 · load · 每核柱 · 5 分钟曲线）、内存（使用率 · 已用/总量 ·
    可回收缓存 · swap · 5 分钟曲线）
  - 右子列：网络（↑↓ 速率 · 丢包/错包 · 双曲线）、磁盘 I/O（读/写速率 · 双曲线）、磁盘用量（使用率 ·
    剩余/总量 · 条）、容器/线程/进程数、累计流量、PSI 压力
  - 曲线区带 25/50/75% 极淡刻度线 + 右上角量程，底部一行说明时间跨度（近 5 分钟 · 1 Hz）
- **尾网块**（主机块正下方，横跨主机区）：`tailscaled` 的 localapi（**不 fork CLI、不需要 root**），
  两个子列各 5 台：名字 · 系统 · `直连`/`中继 <DERP区域>` · 此刻收发速率（离线设备显示「离线 · 2h 前」）。
  标题行给「9 台 · 7 在线」「本机 100.64.0.1」「直连 4 · 中继 2」
- **进程榜整列归这一台机**（右侧，约 34% 宽）：**一整列一条榜**，按 CPU% 降序取 25 条
  （同值比 RSS ⇒ 空载时自动变成内存榜），每行带名次 · 名称 · pid · 内存 · CPU%
- **生产服务器**：ssh 长连接 + 远端 `perl` 探针，**不在被监控机装任何东西**（见下）

> 2026-11-06 之前是「本机 │ 生产 / 生产2 │ 进程榜」三列；hpcmd 下线后第二台机归零，
> 双机列 + 三段曲线各占一块的版式会在每张曲线上方留下大片空白 —— 现在是一台机、四张曲线、
> 进程榜一整列。同一天下午又砍掉了「按内存 / 按 CPU 两段进程榜」（同一台机画两遍、
> 用户原话「两个一模一样的进程 列表」）：改成一条按 CPU% 的榜，腾出的高度给主机块下面那块，
> 曲线高度上限从 260 降到 150（「一个折线图占位太多了」）。那一块先放过容器清单，
> 用户随后指出「容器好像不就是进程吗 那算了 不要了 换成别的吧」⇒ 改成 Tailscale 尾网。

### 生产服务器监控（`--remote`）

```
hpopi5b-panel ──fork/exec──▶ ssh -T hpcore 'perl -'  ──▶ 被监控机的 perl（脚本从 stdin 喂进去）
     ▲                          stdin = tools/probe-remote.pl（本地文件，不落盘到对方）
     └──── 非阻塞读 stdout ───── S/C/P/Q/D/E 行（S/C 每帧、P/Q/D 每 3 秒）
```

| 设计点 | 为什么 |
| --- | --- |
| **一条长连接，不是每秒 ssh** | 每次建连 ~100ms + 一次完整握手，还会让远端 sshd 每秒 fork 一个进程 |
| **远端只读 `/proc`、`/sys`** | 不装包、不落文件、不起服务、不开端口 —— 那台是生产机，原则是「必须干净」 |
| **fd 直接进主循环的 `poll`** | 不引入第三个线程，不打断 1 Hz 采样节奏 |
| **只在面板可见时跑** | 桌面态/登录页把子进程停掉（没人看的时候不该有流量），但**保留已采到的数据** |
| **半帧不提交** | `S`/`C`/`P`/`Q`/`D` 写进暂存，收到 `E` 才整体提交 ⇒ 视图读不到「CPU 是新的、进程榜是空的」 |
| **退避重连** | 2→4→8→16→30 秒封顶；断线时列标题旁显示「离线 Ns」，主循环照常跑 |

实测成本：远端 `perl` **~0.3% of one core**，本机 `ssh` 进程 <0.1%。
配置写在 `/etc/hpopi5b-panel.conf`（`remote=hpcore`），优先级 `--remote` > `$HPOPI5B_REMOTE` > 该文件。
**没配就画本机**（标题写「本机」，不拿别的机器的数据冒充）。

> `remote2=` / `--remote2` 自 2026-11-06 起**已废弃**：仍被接受（老启动脚本不会炸），
> 但只打一行提示、不再有第二个主机列。

## 为什么是「自己画」——这是实测选出来的，不是偏好

同一台机、真桌面、同一份 1 Hz 工作量：

| 路线 | 实现 | PSS | CPU | 线程 |
| --- | --- | --- | --- | --- |
| **本项目** | **X11 + cairo + pango（C，无工具包）** | **7.4 MiB**（最小程序） | 0.08% | **2** |
| 轻量包 | FLTK | 8.4 MiB | 0.08% | 2 |
| 现成 | conky | 13.4 MiB | 1.33% | 6 |
| 工具包 | GTK3 + C | 17.6 MiB | 0.08% | 5 |
| 工具包 | Qt6 Widgets | 27.4 MiB | 0.42% | 3 |
| 工具包 | GTK3 + Python | 37.1 MiB | 0.08% | 5 |
| 工具包 | **GTK4 + C** | **119.3 MiB** | 0.50% | **22** |

**结论**：内存的大头是**工具包**，不是语言（C 14.1 MiB vs Rust 14.5 MiB，只差 0.4 MiB）。
所以这个项目用 C + cairo + pango，**坚决不引工具包**。

自绘不吃文字质量的亏：cairo + pango 就是 GTK 底下那同一套（cairo/pango/fontconfig/harfbuzz）。
实测只给一个「只有拉丁字形」的字体族（DejaVu Sans）画「中文测试 汉字」，再换 Noto CJK 重画，
**只差 112 像素（0.001%）** ⇒ 字体回退生效，中文与 Nerd Font 图标正常。
另外自绘能精确 `cairo_scale(1.48)` 对上本机 141.8 DPI（GTK 在 X11 只支持整数倍缩放）。

**本程序实测（真机）**：`PSS 24.8 MiB`（其中 **Pss_Anon / Private 仅 5.4 MiB**，其余是与
X/picom/pango 共享的库页）· 2 线程。
CPU 在加第二台主机后从 **0.60% → 约 1.0%（只本机）/ 约 1.4%（含生产服务器）of one core**
（一帧绘制 5~10ms、1 Hz 重绘；实测瓶颈是 pango 整形次数与整屏渐变填充，两者本轮都已优化：
右对齐复用同一次排版、背景渐变只渲染一次再 `XCopyArea` 复制）。
⚠️ 早期文档里的 0.60% 是在「只有一台主机、进程榜 40 条」的版式下量的，与现在不可直接比较。

## 这不是给谁用的（我为什么这么写）

**这个仓库是我方案的展示，不是通用面板框架。** 它每一行都长在下面这些前提上，换台机器基本都得改：

| 前提 | 具体是什么 | 换环境要改什么 |
| --- | --- | --- |
| **屏幕** | 1920×1080 / 141.8 DPI ⇒ `SCALE=1.48`，版式常量按 **1297×729 逻辑像素**调的 | 改 `--scale` 或 `src/theme.h` 的 `L_*` 常量；换分辨率要重新调版式 |
| **桌面栈** | lightdm(autologin) + bspwm + polybar + picom；面板是 `override-redirect` 全屏覆盖层 | 没有 autologin 的机器上「回桌面」这一步不成立；堆叠/抬窗逻辑要重验 |
| **认证** | 需要**专用 PAM 服务** `/etc/pam.d/hpopi5b-auth`（只留 `pam_unix`）；setuid 助手装到 `/usr/local/libexec/` | ⛔ **绝不要复用 `login` 栈**（带 `pam_faillock` ⇒ 会把 autologin 一起锁死，我们实测踩过）；换发行版要自己写这份 PAM 配置 |
| **会话入口** | `/usr/local/bin/hphelm-session` 里起面板 + 守护循环 + 日志轮转 | 得自己接进你的 DM/会话 |
| **运维红线** | 「永不向 lightdm 发锁请求」「守护循环只把运行 <5 秒当失败」「回滚先停 VT 守卫」 | 这三条是我们机队用**血的教训**换来的（见 `docs/pitfalls.md`），直接抄代码但不知道这三条会踩同一个坑 |
| **字体** | JetBrainsMono Nerd Font（图标 + 中文回退靠 fontconfig） | 换字体要重验宽度，版式是拿它量的 |
| **数据源** | Linux `/proc`、`/sys`：`/sys/block/*/stat` 整盘统计、可选 hwmon 温度、zram swap、`/sys/class/net/<iface>` | 设备名与温感是**本机硬件**（虚拟机常无 hwmon）；非 Linux 不成立 |
| **机队口径** | 顶栏那个 `24×7` 标记、tailscale 状态、进程榜按 RSS……都是**我们机队**的看板语义 | 这些不是通用概念，别当成「面板标配」 |

一句话：**「自绘、无工具包、低占用」这套方法可以拿走；「这份代码」拿走要付适配成本。**

## 构建与部署

```sh
make                 # 需要 pkg-config: x11 x11-xcb xscrnsaver cairo cairo-xlib pangocairo
sudo make install    # → /usr/local/bin/hpopi5b-panel
                     # → /usr/local/libexec/hpopi5b-auth（4755 root:root，setuid）
```

⚠️ **构建必须在 `hpopi5b` 上做**（需要 X 开发头文件；`hpcore` 这台源码机**没有 gcc/make**，
`hpcmd` 时代的「本机构建 + `tools/deploy.sh`」已经不适用）。流程是：hpcore 源码 → scp 到 hpopi5b →
`make panel` → `install` 二进制与探针 → `systemctl restart hpopi5b-kiosk`。

部署到 `hpopi5b`（kiosk）的完整流程（含红线守卫、三层回滚、六件绝不能做的事）见 **[`docs/deploy.md`](docs/deploy.md)**；
一条命令的版本是 `tools/deploy.sh`（`--check` 只读；它假定**在本机就地部署**，从 hpcore 推过去要走
`docs/deploy.md` 的 scp 路径）。

常用参数：

```sh
hpopi5b-panel --panel-idle 180     # 桌面空闲多久进面板（秒）
hpopi5b-panel --login-back 120     # 登录页空闲多久回面板（秒）
hpopi5b-panel --input-grace 1.2    # 进面板后多久才接受唤醒（防误触）
hpopi5b-panel --sleep-off 0       # 面板空闲多久关屏（DPMS）；**0 或负数 = 永不关屏（默认）**
hpopi5b-panel --scale 1.48         # 缩放（物理/逻辑像素比）
hpopi5b-panel --remote hpcore      # 被监控机（ssh 目标）；空 = 只画本机
hpopi5b-panel --lock /tmp/x.lock   # 单实例锁路径（默认 /tmp/hpopi5b-panel.lock）
hpopi5b-panel --state panel|login  # 强制初始状态（验证界面用）
hpopi5b-panel --kiosk             # 纯显示器：永不进登录页/桌面（Pi 看板用）
hpopi5b-panel --remote2 hpcore    # ⚠️ 已废弃（2026-11-06 单机化）：接受但忽略，只打一行提示
hpopi5b-panel --scene panel|trending|news  # 钉住一幕（不轮换）
hpopi5b-panel --rotate 30         # 幕轮换间隔（秒）；0 = 不轮换（kiosk 默认 30，桌面默认 0）
hpopi5b-panel --transition dip|cut  # 转场：dip=淡出淡入（默认）· cut=硬切（验版式用）
hpopi5b-panel --theme auto|day|night  # 主题：auto=按时刻自动切（默认）· day/night 钉住
hpopi5b-panel --theme-day 07:00   # 白天起点（默认 07:00）
hpopi5b-panel --theme-night 19:00 # 夜间起点（默认 19:00）
hpopi5b-panel --list-scenes       # 列出可用幕后退出（不需要 X）
hpopi5b-panel --dump               # 只打采集层数据后退出（不需要 X）
HPOPI5B_PANEL_DEBUG=1 …            # 主循环仪表：it/s · poll 结果 · 事件类型 · 各阶段耗时
```

### 多幕（一块屏不只显示一种东西）

面板是**幕**的一个实现，目前三幕：`panel`（单机双子列 + 尾网块 + 进程榜）· `trending`（GitHub 趋势，
**流式两列**：按每条描述实际占 1~2 行算高度、再把两列零头均摊进行距（上限 8 像素），标题行给
「N 分钟前同步 · 共 M 条 · 今日 +X★」，右列尾部挂一行**语言分布**脚注（计数 + 单色相 alpha 递降堆叠条））。
`--kiosk` 下默认每 **30 s** 淡出淡入换一幕；桌面/锁屏模式默认**不轮换**。
加一幕 = 新建 `src/scene_*.c` + 在 `scenes.c` 的表里加一行，别的不动。

### 白天 / 夜间主题

固定时刻自动切：**07:00 之后白天、19:00 之后夜间**（两个时刻都能改，见参数表/配置文件）。
切换是「淡出到黑 → 换色 → 淡入」，全程 0.9 s，不会闪白也不会硬切。登录页同样跟随主题。

主题**只改配色与亮度**，不碰内容：所有绘制代码用的都是语义宏（`C_MAUVE` 永远是 CPU/主色），
色值只在 `src/theme.c` 的两张调色板里（白天 Catppuccin Latte / 夜间 Mocha）。
所以加一幕不用写两遍配色，也不会出现「某一幕忘了换主题」。

`trending` 的数据**不是 API**：GitHub 的 trending 只存在于网页里（官方没有接口），
所以由 `tools/fetch-trending.py` 抓 `github.com/trending?since=daily` 解析成 TSV，存到
`/var/cache/hpopi5b-panel/trending.tsv`，面板**只读这个文件**。`hpopi5b-trending.timer` 每 3 小时跑一次
（`OnUnitActiveSec=3h`，开机 2 分钟后先跑一次）；抓不到就保留上一份，**网络故障不影响渲染**。
「疑似半页」（结尾不是 `</html>`、`article` 条数与 `stars today` 处数对不上、条数比上一份骤降）会重抓一次；
两次都可疑就不覆盖缓存并记 journal（两次一致则当真——GitHub 自己也会给短榜）。
换榜单：改 `systemd/hpopi5b-trending.service` 的 `ExecStart` 追加 `--since weekly` 或 `--language rust`。

`news`（新闻，**三列**：左「国内热点」13 条带热度 `1428万` 和一条量度杆、右「科技」28 条分两子列带「来源 / 1d」）。
**没有列标题行**（分组靠列宽比例 + 竖细线 + 首条放大），版式规则见
[`docs/design.md`](docs/design.md) 的「五·五 榜单幕」。
同样是「定时抓 → 缓存 TSV → 幕只读文件」：`tools/fetch-news.py` 每 **30 分钟**（`hpopi5b-news.timer`）
抓**头条热榜**（退化到百度热搜）当国内热点、抓 **Solidot + 少数派 + 36氪** 三路 RSS 合并按时间倒序
当科技，原子写 `/var/cache/hpopi5b-panel/news.tsv`。选源理由与实测（微博/知乎是登录墙、人民网/新浪 RSS
是僵尸）见 [`docs/architecture.md`](docs/architecture.md) 的「新闻幕」节。

> 热搜是**没有分类字段**的（头条/百度都只有 标题 + 热度），所以左列必然掺体育/娱乐——这是数据源的
> 形状，不是排版问题。想要「只挑某类」，正确做法是换源（比如只留 `chinanews` 的滚动 RSS），不是过滤热搜。

轮换间隔只有一个数 `--rotate`（或 `/etc/hpopi5b-panel.conf` 的 `rotate=`）：30 s 的来历是
「密排面板抬头到看完约 8~12 s，留 2~3 倍余量」；现在 3 幕 = 90 s 一圈，4 幕就到 2 分钟，再长值班的人会漏掉
告警页。真要放很多幕，正确做法是「告警幕常驻 + 其余轮换」，不是把间隔压到 5 s。

## 文档地图

| 你想干什么 | 先读 |
| --- | --- |
| **接手维护这个项目**（AI/人都一样） | **[`AGENTS.md`](AGENTS.md)** ← 工作流、红线、验收判据、不要做什么 |
| 看逐轮交接与当前待办 | [`HANDOFF.md`](HANDOFF.md) |
| 改 UI / 版式 / 配色 | [`docs/design.md`](docs/design.md)（设计系统 + 版式常量 + 曲线规范） |
| 搞清状态机 / 双缓冲 / 认证模型 / 数据采集 | [`docs/architecture.md`](docs/architecture.md) |
| **看不到图时怎么验 UI**（本项目最关键的方法论） | [`docs/verify.md`](docs/verify.md) |
| 部署 / 重启 / 回滚 | [`docs/deploy.md`](docs/deploy.md) |
| 已踩过的坑（14 条，每条带判据） | [`docs/pitfalls.md`](docs/pitfalls.md) |

## 目录结构

```
src/theme.c   两套调色板 + 固定时刻判定（语义色只在 theme.h 里声明）
src/theme.h         配色（Catppuccin Mocha）+ 版式常量（逻辑像素）
src/metrics.[ch]    本机数据采集：只读 /proc、/sys，不 fork 子进程
src/remote.[ch]     生产服务器采集：ssh 长连接 + 非阻塞行解析 + 退避重连
src/view.[ch]       绘制基元：文字/背景渐变/细线/进度条/每核柱/sparkline/量程量化
src/view_panel.c    面板版面（无卡片、主机区两子列 + 尾网块 │ 一整列进程榜）
src/view_login.c    登录页版面（无卡片、居中）
src/scene.h         幕的接口：draw + 自己的内容签名（不碰 X）
src/scenes.c        幕的注册表 + 轮换调度 + 转场推进（纯逻辑）
src/scene_panel.c   幕「运维面板」（转调 view_panel.c）
src/scene_trending.c 幕「GitHub 趋势」（只读缓存 TSV，缺文件时给空态；流式两列 + 排版计划缓存）
tools/fetch-trending.py GitHub Trending 抓取器（无依赖，python3；防半页：可疑重抓一次，两次都可疑则不覆盖缓存）
src/scene_news.c   幕「新闻」（三列：国内热点 1 列 + 科技 2 列，两行式 44px + 首条 92px；只读缓存 TSV，缺文件时给空态 + 抓取命令）
tools/fetch-news.py  新闻抓取器（头条热榜 + 百度热搜 + 3 路 RSS，无依赖，python3）
systemd/        hpopi5b-trending.{service,timer}（每 3 小时同步一次榜单）· hpopi5b-news.{service,timer}（每 30 分钟）
src/auth.[ch]       与 setuid 助手通信（密码只走 stdin）
src/hpopi5b-auth.c   唯一的特权代码：setuid root，调 PAM 验一次密码
src/panel.c         X11 窗口、事件、状态机、主循环、双缓冲
tools/              渲染验证（render-ascii/render-region/measure/pngcrop）、帧差分探针、
                    窗口截图（xshot.c）、部署脚本、
                    probe-remote.pl（喂给被监控机的只读探针）
docs/               架构 / 设计 / 验证 / 坑 / 部署
```

## 状态与边界

- **活跃**：这是 `hphelm` 上正在跑的方案（看板 + 锁屏都是它）。
- **不做**：不做成通用面板、不做配置系统、不做主题系统、不引 GUI 工具包、不追跨平台。
  想「通用化」的改动一律先问「这是不是把我这台机的假设包装成了产品」。
- **没有 license 文件** = 保留所有权利（和我其它仓库一致）。要拿去用请先读上面那张适配表，
  或者直接把它当成「一个具体方案长什么样」的参考。

## 相关

- 机队视角的运维记录（部署位置、VT 守卫、红线）：`kb` 仓库的 `runbooks/hpopi5b-panel.md`
  （本机副本 `/data/kb`，主副本在 `hphelm:/data/kb`；旧形态散在 `runbooks/hphelm.md` 的「运维面板」节）。
  **那是机队事实的权威来源**，本项目文档负责代码与设计。
- 项目记忆库：`.ai/`（`engram dump` 出 ≤1.5 KB 会话指纹；`engram doctor` 查漂移）。

## 真实可移植的做法

想把「自绘、无工具包、低占用」用到自己机器上，**照方法走，不要照抄代码**：

1. **先量再选**：在你自己的机器上复现上面那张选型表（同一份 1 Hz 工作量，比 PSS/CPU/线程）。
   结论很可能是「工具包才是大头」——那就自绘。
2. **渲染骨架**：离屏 pixmap + 每帧一次 `XCopyArea`（不要直接画窗口，会闪）；
   GC 记得关 `graphics_exposures`（否则主循环自激空转）。细节见 `docs/architecture.md`。
3. **认证**：写一个「只做一件事」的 setuid 助手（读 stdin 密码 → PAM 验一次 → 打印 OK/FAIL），
   配一个专用 PAM 服务。**绝不复用 `login` 栈**（会连坐 autologin）。
4. **版式**：先定「逻辑像素」坐标系和一组 `L_*` 常量，再画。
5. **验证**：准备三个探针——整屏 ASCII、几何测量（色块游程/墨迹带）、帧差分。看不到图也能改 UI。

⇒ 这份代码的价值在于**它记录了我们踩过的坑与判据**（`docs/pitfalls.md` 14 条）。
