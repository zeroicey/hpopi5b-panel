# 架构：状态机 · 双缓冲 · 认证 · 数据采集

> 这份文件回答「它怎么跑起来的」。改交互、改渲染、加指标之前读它。
> 相关：[`design.md`](design.md)（版式与配色）· [`verify.md`](verify.md)（怎么验）· [`pitfalls.md`](pitfalls.md)（踩过的坑）

---

## 一、状态机（`src/panel.c`）

```
DESKTOP ──idle≥panel_idle(180s)──▶ PANEL ──input(≥input_grace 1.2s)──▶ LOGIN
   ▲                                                                    │
   └────────────────── 认证成功（PAM，见第四节）────────────────────────┘
LOGIN ──idle≥login_back(120s)──▶ PANEL（密码框清空、清错误）
PANEL ──idle≥sleep_off(0=永不)──▶ DPMS 关屏；任意输入唤醒（并进 LOGIN）
```

> **`sleep_off` 的 0 语义（2026-10-19）**：默认改成 **0 = 永不关屏**（用户要求看板 7×24 一直亮）。
> 实现上必须写 `CFG_SLEEP_OFF > 0 && idle >= CFG_SLEEP_OFF` —— **裸写 `idle >= CFG_SLEEP_OFF`
> 会让 0 变成「立即关屏」，语义正好反过来**（这是本轮改之前就埋着的坑）。

**空闲时间怎么来**：`XScreenSaverQueryInfo(dpy, root, info)` 的 `info->idle`（毫秒）。
**不要自己数鼠标事件** —— 面板只在自己显示时收到事件，桌面态收不到，`XScreenSaver` 是唯一的全局口径。

**三个状态的显隐语义**：

| 状态 | 窗口 | 键盘 | 焦点 | 备注 |
| --- | --- | --- | --- | --- |
| DESKTOP | unmapped | **必须 `XUngrabKeyboard`** | 还给 `PointerRoot` | 忘了放 grab ⇒ 整个桌面收不到按键（踩过） |
| PANEL | mapped + raised | grabbed | 面板窗口 | 每 2 tick `XRaiseWindow`（防 polybar 后映射压上来） |
| LOGIN | mapped + raised | grabbed | 面板窗口 | 同上 |

**信号入口**（运维用）：`kill -USR1` = 立即进 LOGIN；`kill -USR2` = 立即回 PANEL。

**开机必须 `--state panel`**：开机 idle=0，若从 DESKTOP 起步就会直接暴露桌面、丢掉「要密码」。

**单实例锁**：`flock /tmp/hpopi5b-panel.lock`（`--lock PATH` 可改）；已有实例则 `exit 3`，
会话包装器见到 `rc=3` 会 `sleep 30` 避免空转。
（两个实例会互相抢抬窗 ⇒ 屏幕变成两张界面互盖，而日志一切正常 —— 踩过。）

---

## 二、渲染：双缓冲 + 内容签名

```
每帧：
  content_sig()  ── FNV-1a over 本机 metrics_t + 远端快照 + 界面态
      │            （state/pwlen/busy/cursor/err/倒计时/远端在线与离线秒数）
      │ 没变 ⇒ return（不画、不上屏）
      ▼
  render()       ── ① XCopyArea(bgpix → back)   背景渐变只渲染过一次
                    ② 全部绘制进离屏 Pixmap（back）
                    ③ cairo_surface_flush
      ▼
  blit()         ── 一次 XCopyArea(back → win)    ──▶ XFlush
```

**两个 pixmap 分工**：`bgpix` 存渐变底（尺寸变化时重建一次），`back` 是每帧的后备缓冲。
每帧先用一次服务器端 `XCopyArea` 把底铺上去，再让 cairo 在最上面画内容 ——
比每帧让 cairo 求值一遍整屏渐变便宜 4~5ms（见 `design.md` 六·五）。

### 为什么必须离屏（否则会闪）

最初是 `cairo_xlib_surface_create(dpy, win, …)` 直接画窗口。每帧顺序是「先 `cairo_paint(C_BASE)`
把整屏涂底色 → 再逐块画卡片/文字」，cairo 分批把请求发给 X 服务器，**中间态是真实可见的**
（picom 开了 `use-damage=true`，随时可能抓到「刚清完底、还没画完」的那一帧）⇒ 每秒闪一下。

实测判据：抓窗口连续帧逐像素差分 ⇒ 每秒两次「整屏 63% 像素变化」，中间帧 PNG 只有 67KB
（前后 177KB）且只有底色没有盒子/文字。

### 三个实现细节（都别回退）

1. **GC 必须关 `graphics_exposures`**：
   ```c
   XGCValues gcv; gcv.graphics_exposures = False;
   blitgc = XCreateGC(dpy, win, GCGraphicsExposures, &gcv);
   ```
   默认 `True` ⇒ 每次 `XCopyArea` 回一个 `NoExpose` ⇒ `poll()` 立刻返回 ⇒ **主循环自激空转**
   （实测 35 it/s、CPU 0.8%→33~49%、采样窗口压到 ~28ms ⇒ 面板上的 CPU% 跳成 KTV）。

2. **窗口背景 = 后备 pixmap**（`XSetWindowBackgroundPixmap`）⇒ 暴露/映射时服务器自己回填，不闪黑。

3. **`win_show()` 顺序**：先 `render()` → `XMapRaised` → `XSync` → `blit()` → `XSetInputFocus` → `XGrabKeyboard`。
   ⚠️ 顺序不能换：对**未映射**窗口调 `XSetInputFocus` 会 `BadMatch` 并**直接终止进程**（`Major opcode 42`）。

尺寸变化（`ConfigureNotify`）⇒ `surface_init()` 重建 pixmap + `redraw_force()`。

---

## 二·五、生产服务器采集（`src/remote.[ch]` + `tools/probe-remote.pl`）

```
hpopi5b-panel ──fork/exec──▶ ssh -T <target> 'perl -'   ──▶ 被监控机的 perl
     ▲                         stdin = tools/probe-remote.pl（本地文件）
     └─── 非阻塞 read(stdout) ── 1 Hz 的 S/C/P/Q/E 行
```

**探针输出契约**（改任何一边都要同时改另一边）：

| 行 | 内容 |
| --- | --- |
| `S` | `ts host cpu% ncore uptime memT memU swapT swapU disk%:GB rx tx temp l1 l5 l15 freqMHz dock nproc gov iface ip` |
| `C` | `core0% core1% …`（ncore 个） |
| `P` | `nproc pid:rssMiB:cpu%:name,…`（每帧，按 RSS 取 24 条） |
| `Q` | `pid:rssMiB:cpu%:name,…`（每 3 秒，按 CPU 降序取 **40** 条，**无阈值** ⇒ 空载时自动退化成内存榜） |
| `T` | `总台数 在线数 本机IP name:os:直连0\|1:DERP区域:在线0\|1:离线秒:rxBps:txBps,…`（每 3 秒，在线优先再按流量降序取 **10** 台；本行来自 tailscaled 的 localapi） |
| `E` | 一帧结束 ⇒ **只有收到 E 才提交** |

`P`/`Q`/`T` 的多字段格式是 2026-11-06 起的新格式；面板这边按**冒号段数**兼容老的 3 字段
`P` 格式（老探针没有 CPU% ⇒ `cpu_pct = -1` ⇒ 该列显示「-」，不显示假 0）。
`T` 行来自 **tailscaled 的 localapi**（`GET /localapi/v0/status`，走 `/var/run/tailscale/tailscaled.sock`，
权限 0666 ⇒ 不需要 root），**不 fork `tailscale status`**（那要起一个进程、几百毫秒，1 Hz 的探针扛不住）。
⚠️ JSON 里对端列表的键是 **`Peer`（单数）**——写成 `Peers` 会**静默**得到 0 台。
`直连0|1` 由 `CurAddr` 是否为空判定；`离线秒` 由 `LastSeen`（RFC3339 带纳秒）算；速率是相邻两次
`RxBytes`/`TxBytes` 累计值的差除以帧间隔 ⇒ 与 `Q` 行同为**瞬时**值。tailscaled 临时不可用时探针沿用上一帧的
`T` 行（宁可数据旧，不要闪空）。
`cpu%` 是「上一帧到这一帧」的瞬值，1.0 = 占满一个核，进程可以是 300%（多线程）。
探针内部按 `pid → 上帧 utime+stime` 记忆差值算 CPU%（`USER_HZ` 恒 100、帧间隔 <1s 时不算），
所以 `Q` 行是「上一帧到这一帧」的瞬时占用，不是平均值。
面板不再画「按内存」那一段（用户说「两个一模一样的进程列表」），`P` 行留着只作调试用。

**四条设计约束**：

1. **不 fork 每帧**：建连一次、常驻，fd 直接进主循环的 `poll`（不引入第三个线程）。
2. **只在面板态跑**：`ST_PANEL` 才 `remote_start()`，回桌面/登录页 `remote_stop()`
   （没人看的时候不该有网络流量与远端 CPU），但**保留已采到的快照** ⇒ 回面板立刻有整帧可画。
3. **半帧不提交**：`S`/`C`/`P` 写进 `stg`，收到 `E` 才 `m = stg` ⇒ 视图读不到撕裂状态。
4. **退避重连**：2→4→8→16→30 秒封顶；断线期间列标题旁写「离线 Ns」，主循环照常。

**四个已踩的实现坑**：

- `P`/`Q`/`D` 行**必须按逗号分段**，不能按空格：进程名里有空格（`searxng worker`），
  按空格分会把一条记录劈成两半，名字里混进 pid 和内存数字（实测）。
  探针那边也把名字里的空白/逗号/冒号替换成 `_` —— 两头都防。
- `stop_child()` **不能 kill 完就把 pid 清零**：子进程会变僵尸且永远没人收。
  现在给 200ms 宽限（只在状态切换时发生）再从 `SIGKILL` 兜底。
- **「连上了但没数据」必须也超时重连**：我第一版只在 `saw_frame && age > 30` 时掐掉，
  于是「ssh 连上了、远端 perl 却没起来」（对方刚重启 / 探针挂了 / 卡在认证）会**永久挂住**，
  面板永远显示「离线」——2026-10-21 真机就这么卡了一次。现在：
  从未收到帧 ⇒ 连接存活 20 秒即视为失败；收到过帧 ⇒ 30 秒无新帧视为失败。
- 探针脚本用 `ssh 'perl -' < script` 喂进去，**不落盘到对方机器**；
  本程序找脚本的顺序是 `$HPOPI5B_PROBE` → `/usr/local/share/hpopi5b-panel/probe-remote.pl` → 开发目录。

**运行态必须有状态变迁日志**（`rlog()`）：拉起 / 已连上 / EOF / 进程退出 / 超时重连，
每次变迁一行。没有它的时候，线上「离线」只能靠猜 —— 2026-10-21 就是这么排查了半小时。
正常一天几行，不刷屏。

**`--dump` 会真的拉一帧**（部署自检靠它判「链路通」）。⚠️ 配置行必须是 `--dump` 输出的**最后一行**
以外的位置按前缀取 —— 我第一版让部署脚本用 `tail -1`，加一行日志就把判据搞成了假失败。

## 二·六、多幕（scene）与转场（`src/scene*.c`、`src/scenes.c`）

面板不再是「一个写死的版面」，而是**幕**（scene）的一个实现。接口在 `src/scene.h`：

```c
typedef struct {
    const char *id, *label;
    void (*draw)(cairo_t *, const view_ctx_t *, const scene_env_t *);
    unsigned long long (*sig)(const scene_env_t *);
} scene_t;
```

- `draw` 收到**已经 `cairo_scale` 过的**上下文，底图（`bgpix`）也已经 `XCopyArea` 到目标缓冲上；
  幕只画自己的内容，不铺底、不碰 X。
- `sig` 是**幕自己的内容签名**，与 `panel.c` 里原有的 `content_sig()` 相与。`panel` 幕哈希
  `metrics_t`（1 Hz 变化）；`trending` 幕哈希「分钟」（「N 分钟前同步」要跟着走）——缓存文件没变时
  一分钟才重画一次，省电也避免无意义刷新。
- 调度只有**一个数**：`--rotate S`（默认 kiosk 30s、桌面/锁屏 0=不轮换）。不搞每幕不同停留。
  已经出现「信息密度差很多」的幕时，再给 `scene_t` 加 `dwell` 字段 —— 现在不要提前设计。

**双缓冲 + 转场**：`back` 是最终帧，转场**只有一种**：

| 转场 | 做法 | 代价 |
| --- | --- | --- |
| `dip`（默认） | 前半画 A、后半画 B，整屏叠一层 `crust` 色（alpha 0→1→0） | 每帧 1 次整屏 `cairo_paint` |
| `cut` | 直接画目标幕，没有中间帧（只用于验版式） | 0 |

> 2026-11-05 撤掉 `push`/`cover`：平移要把两幕分别画进两块 8 MiB 缓冲再做切片，
> 每次切幕多一次全屏绘制，视觉上也不如淡变干净。`dip` 只需要一块缓冲 + 纯色覆盖。

转场时长固定 0.55 s；主循环在转场期间把 `poll` 超时从 500 ms 压到 **30 ms**（≈ 33 fps 上限），
结束后恢复 500 ms。性能预算：0.55 s × 一次整屏纯色覆盖 ≈ 10 ms，**摊到 30 s 轮换周期上 ≈ 0.03%**。

**回到面板时**（`set_state(ST_PANEL)`）调 `scenes_resume(now)`：打断进行中的转场、把停留计时
从现在重新开始 —— 否则「登录页待了两分钟，一回到面板立刻又换幕」。

**`trending` 幕的数据通路**（不自己联网，靠 timer 喂文件）：

```
fetch-trending.py（urllib + regex，GitHub 没有 trending API）
     └─ 原子写 /var/cache/hpopi5b-panel/trending.tsv   ← systemd timer 每 3h
              │
scene_trending.c  stat() 比对 mtime/size，文件变了才重读；sig 里混进「分钟」
              └─ 无文件 = 空态（写清楚该跑哪个 service，不装假数据）
```

这样做的理由和 `remote` 一样：**网络与渲染隔离**。抓取失败/超时只会让榜单停在上一份，
面板永远不需要为 HTTP 等一秒。TSV 而不是 JSON，是为了让 C 侧零依赖（`fgets` + `\t` 切列）。

**`news` 幕的数据通路**（同一条模式；**一个抓取器出两个源**，幕上排成三列）：

```
fetch-news.py（每 30min，hpopi5b-news.timer）
     ├─ 国内热点：头条热榜 API（JSON）→ 失败退化到百度热搜 API
     ├─ 科技：Solidot + 少数派 + 36氪 三路 RSS 合并、去重、按 pubDate 倒序
     └─ 原子写 /var/cache/hpopi5b-panel/news.tsv（H\t标题\t源\t热度 / T\t标题\t源\tunix_ts）
              │
scene_news.c  stat() 比对，变了才重读；sig 混「分钟」（「N 分钟前同步」）
              └─ 无文件 = 空态（写清楚 service 名与缓存路径）
```

⚠️ **`news` / `trending` 的 `scene_t.live = 0`**：它们只吃外部文件，`panel.c` 不会把 1Hz 指标快照混进
全局内容签名；否则每秒一次整屏重绘（实测 news 幕一次绘制 ~20 ms ⇒ 白烧 1% 单核，见 `docs/pitfalls.md` 第 24 条）。

**源是怎么选的**（2026-11-08 在 hpopi5b 上逐条实测，不是查文档）：

| 源 | 结果 |
| --- | --- |
| 头条热榜 `hot-event/hot-board` | ✅ 200、50 条、带热度值 —— 左列首选 |
| 百度热搜 `top.baidu.com/api/board` | ✅ 200、51 条、带热度，**无分类字段** —— 退化方案 |
| Solidot / 少数派 | ✅ 200 —— 右列主力（Solidot 裸抓 403，需带 UA） |
| 36氪 `36kr.com/feed` | ⚠️ 200 但**解析出 0 条**（feed 结构特殊），留着待修 |
| chinanews / 界面 RSS | ✅ 200、30 条、pubDate 新鲜 —— 备选（想要「只挑时事」就换这个） |
| 微博热搜 / 知乎热榜 | ❌ 403 / 401，m.weibo.cn 直接返回「Sina Visitor System」 |
| 人民网 / 新浪 / 新华社 RSS | ❌ 200 但 pubDate 停在 2025 或只剩 1 条（僵尸源） |
| Google News / HN 官方 RSS / V2EX / RSSHub | ❌ 000（TLS 或注册墙 / 超时） |

**主题（白天/夜间）**

```
theme.h   语义宏 C_MAUVE/C_TEXT/… → TP.<field>（运行期可变的调色板）
theme.c   两张 palette_t（Latte / Mocha）+ theme_night_at(t, day_min, night_min)
panel.c   每 10s 看一次钟；到点 → 淡出(0.45s) → 换色+重画 bgpix → 淡入(0.45s)
```

三个容易踩的点：

1. **`bgpix` 里烤的是渐变**，换主题必须 `bg_init()` 重画；只换宏不会动那一层。
2. 淡变遮罩画在**后备 pixmap 上**（`cairo_paint` 全黑 + alpha），不是画在窗口上，
   所以仍然只有「一帧一次 `XCopyArea`」上屏，不会撕裂。
3. 主题不进 `content_sig()`：淡变期间强制失效，静下来后签名恢复去重。

**统一 HUD**：`render()` 在所有幕之后画底部居中的几个小点（当前幕实心），只有轮换打开时才画。
幕自己不画页码（右下角的页码点由 `panel.c` 统一画）。

## 三、采样：为什么一切都按「时间」节流

`metrics_sample(&M, want_procs)` 只读 `/proc`、`/sys`，**不 fork 子进程**。约定：

| 项 | 频率 | 为什么 |
| --- | --- | --- |
| 全量采样 | **1 Hz**（主循环按 `now_s()` 节流） | 采样周期 = CPU/网络速率的计算窗口 |
| 进程榜（`read_procs`，遍历 `/proc`） | 每 3 秒（`nsamp % 3`） | 面板里最贵的一项；实测 3 秒一刷无可测影响 |
| `getifaddrs`（IP） | 每 10 秒 | 最贵的一次系统调用（netlink 全量转储），IP 几乎不变 |
| 显示刷新 | 内容变了才画（时钟每秒变 ⇒ 稳定 1 Hz） | 见上一节的内容签名 |

**`dt` 用单调时钟（亚秒）**，不要用 `time_t` 的整秒差：采样快于 1 Hz 时 `difftime` 恒为 0
⇒ 网络速率永远算不出来（踩过，当时是自激空转把循环带到 35 Hz）。

**性能细节**：
- 别「每核打开一次 `/proc/stat`」——单次遍历拿汇总 + 所有核（8 核省 8 次 fopen）。
- 进程榜只读一次 `/proc/<pid>/stat`（`(comm)` 与 rss 都在里面），比 comm+statm 少一半 fopen。
- `read_procs` 的 `all[512]` 必须是 `static`（别把 20KB 放栈上，这函数每秒都调）。

---

## 四、认证：唯一的特权代码

```
面板（普通用户） ──写密码到 stdin──▶ hpopi5b-auth（setuid root, ~120 行）
                                        │ pam_start("hpopi5b-auth") → pam_authenticate → pam_acct_mgmt
                                        ▼
                                   stdout: OK / FAIL 原因
```

| 项 | 内容 |
| --- | --- |
| 位置 | `/usr/local/libexec/hpopi5b-auth`（`-rwsr-xr-x root:root`） |
| 做什么 | **只做一件事**：从 stdin 读一行密码 → PAM 验一次 → 输出 `OK`/`FAIL 原因`。**不开 shell、不执行命令、不写文件** |
| 限制 | `argv[1]` **必须等于 `getuid()` 对应的用户名** ⇒ 就算被滥用也不能横向验证别人 |
| 密码安全 | **只走 stdin**：不进 argv（`ps` 可见）、不进 env（`/proc/pid/environ` 可见） |
| PAM 服务 | ⚠️ **专用** `/etc/pam.d/hpopi5b-auth`，只留 `pam_unix`。**绝不复用 `login` 栈** |
| 防爆破 | 面板自己做指数退避（2/4/8/16/32 秒），与 DM 的 faillock 完全解耦 |

**为什么绝不复用 `login` 栈**：`login → system-local-login → system-auth` 带 `pam_faillock`，
而 `lightdm-autologin` 的第一行也是 `pam_faillock.so preauth` ⇒ 在锁屏上输错 3 次会把账号锁上、
**连坐 autologin 失败** ⇒ lightdm 退回 greeter 并把 VT 切走（踩过，见 `pitfalls.md`）。

**自检（不需要密码）**：
```sh
/usr/local/libexec/hpopi5b-auth                  # FAIL usage
/usr/local/libexec/hpopi5b-auth root             # FAIL 只允许验证调用者本人
echo wrong | /usr/local/libexec/hpopi5b-auth $USER   # FAIL Authentication failure（说明 PAM 真的跑了）
```

---

## 五、数据来源一览

| 指标 | 来源 | 注意 |
| --- | --- | --- |
| CPU 总量/每核 | `/proc/stat` 一次遍历 | 差值算法，窗口 = 采样周期 |
| load | `/proc/loadavg` | |
| 温度（仅文本） | `/sys/class/hwmon/*/name == k10temp` → `temp1_input` | **本机硬件**；KVM 虚拟机常**没有** hwmon（`hpcmd` 就是），Pi 是 `soc_thermal` ⇒ 没读到的机器只不显示文本，不再占一格空白曲线 |
| governor / 频率 | `/sys/devices/system/cpu/cpu0/cpufreq/` | |
| 内存 / swap(zram) | `/proc/meminfo`（`MemAvailable`、`SwapTotal/Free`） | zram 也走 SwapTotal |
| 磁盘用量 | `statvfs("/")` | |
| 磁盘读写 | `/sys/block/<整盘>/stat`（read_sectors / write_sectors × 512，各盘求和）| 跳过 `loop/ram/zram/nbd/sr/fd/dm-/mtdblock`；分区不在 `/sys/block` 下，天然不重复。**不要只认 `nvme*`**（Pi 是 `mmcblk1`、`hpcmd` 是 `vda`） |
| 网络速率 | `/sys/class/net/<iface>/statistics/{rx,tx}_bytes` | 差值 / dt |
| 网卡选择 | 遍历 `/sys/class/net`，**判据是「有全局 IPv4」** | ⚠️ 不能靠 `readdir` 顺序（实测先返回没接线的 `wlp1s0`） |
| IP / tailscale | `getifaddrs` | 最贵 ⇒ 10 秒一次 |
| 进程榜 | `/proc/<pid>/stat`（comm + rss）| RSS 口径，与 htop 一致；不是 PSS（PSS 要扫 smaps，太贵）|
