# 验证：看不到图，怎么确定 UI 是对的

> **这是本项目最重要的方法论文件。** 维护这个项目的 AI 通常**无法看图**（模型不支持图像输入），
> 而用户能直接看屏幕。所以「我觉得这样好看」不算完成 —— 必须给出**可复现的像素判据**。
> 相关：[`design.md`](design.md)（设计值）· [`tools/`](../tools/)（脚本）

---

## 一、四类探针（按用途选）

| 工具 | 用途 | 典型输出 |
| --- | --- | --- |
| `tools/render-ascii.py` | 整屏压成 ASCII（亮度字符 + `C`=彩色） | 一眼看出「哪块空、哪块叠、哪块越界」 |
| `tools/render-region.py` | 局部放大（可 1px 一格） | 看某控件形状/对齐是否合理 |
| `tools/measure.py` | **量几何**：色块游程 / 墨迹带 / 颜色 bbox | 柱宽 88px、间距 5px、行距 57.6px |
| `tools/pngcrop.py` | 纯标准库的裁切/放大/游程（**不需要 magick**，看单行颜色分段用） | 分隔线 x=1092、某行色块分段 |
| `tools/probe-frames` | **抓连续帧做逐像素差分** | 「有无半成品帧」= 「有没有闪」 |

```sh
python3 tools/render-ascii.py   assets/panel.png 22 22
python3 tools/render-region.py  assets/panel.png 1360 80 1900 430 10
python3 tools/measure.py        assets/panel.png --runs 240 --bands
python3 tools/pngcrop.py        assets/panel.png out.png --crop 400,470,200,120 --zoom 5
python3 tools/pngcrop.py        assets/panel.png --runs 300 --minrun 3
DISPLAY=:0 tools/probe-frames   60
```

⚠️ 前三个依赖 **ImageMagick**（`magick`）；hpopi5b 上没装 ⇒ 在真机链路上跑不了。
替代：`tools/xshot` 抓图（纯 X11，见第 6 节）+ `tools/pngcrop.py`（纯标准库）+ `LAYOUT_DEBUG=1` 打印的几何。

---

## 二、判据一：不闪（**每轮必跑**）

**判据**：抓面板窗口连续帧、逐像素比对相邻帧，**`>30%` 像素变化的帧必须是 0**。

⚠️ **kiosk 每 30s 换一次幕（panel ↔ trending ↔ news ↔ weather），那一次的 2 帧必然 100% 变化——不是闪。**
怎么看：范围里当时正好 30s 的整数倍 ⇒ 用三条「闪」指纹中的后两条判（连续两帧都变**且**中间帧 PNG 明显更小）；
要干净的数，就在隔离实例里把轮换关掉量：`DISPLAY=:9 ./hpopi5b-panel --state panel --rotate 0 --lock /tmp/xv9.lock`。
2026-11-05 实测：同一条命令连跑三次得到 `2 / 0 / 3`，差异全来自是否跨越了换幕时刻。

```
$ DISPLAY=:0 tools/probe-frames 60
frame 12 diffpx=       0 ( 0.00%)
frame 13 diffpx=   35124 ( 1.69%)  bbox=(52,177)-(1503,584)   ← 1Hz 内容更新，正常
...
---- frames with >30% pixels changed: 0                      ← 通过
```

**「闪」的指纹**（三条同时出现才是）：

1. 单帧变化 **>30%** 像素，且 bbox 横跨整屏（不是某个小块）；
2. **连续两帧都变**（A→B→A 的振荡，而不是单调更新）；
3. 把中间那帧单独存成 PNG，**文件明显更小**（只有底色/没有文字），像素上「盒子/文字消失」。

实测对照（修双缓冲前）：`1205996`、`1286011` 像素变化 / 每秒 2 次 / 中间帧 67KB vs 前后 177KB。

### 天气幕专用取证（`--scene weather`，截图 + TSV 两头对）

1. `python3 tools/pngcrop.py /tmp/wx.png --runs 300 --minrun 3`：主城温度条/七天气温条/城市条应各有一段
   非背景色游程；数值与 `head -1 /var/cache/hpopi5b-panel/weather.tsv` 的 `cities=5` 对得上。
2. `head -2 weather.tsv` 应有 `P\t广东东软学院\t佛山 · 南海 · 狮山`；`grep -c '^C\t'`=5、`^H\t`=120、`^D\t`=35。
3. 把缓存 `touch -d '40 minutes ago'`，重开 `--scene weather`：顶栏「N 分钟前同步」应变黄（`C_YELLOW`）。
4. 把缓存临时移走：应显示空态（服务名 + 缓存路径），不能是 0°/假曲线；移回后下一分钟自动恢复。
5. 真机 CPU：weather 幕 `live=0` ⇒ 停留时 `重绘 0`；只在换场/分钟边界重绘一次（绘制 ~17ms）。

---

## 三、判据二：不空转（**改了主循环/渲染必跑**）

**判据**：`HPOPI5B_PANEL_DEBUG=1` 下每秒应打印

```
主循环 2 it/s · 事件 0 · poll[可读 0/超时 2/其他 0] · 重绘 1 · 采样 1 · 类型:
```

- `poll` **必须全是「超时」**、事件类型直方图应接近空。出现大量 `NoExpose` / 可读 ⇒ 自激空转。
- `it/s` 应为 2（面板态 poll 500ms）；突然变几十 ⇒ 有东西在叫醒循环。
- `重绘` ≈ 1/秒（内容签名生效）；等于 `it/s` ⇒ 签名没起作用，在无意义重绘。

配套用 `tools/probe-frames` 侧看「内容变化率」：**应为 1 次/秒**。

---

## 四、判据三：几何（**改了版式必跑**）

**原则：设计值是算出来的，实测值必须逐位吻合。**

### 4.1 横向几何 —— 色块游程（run-length）

取某一行，把颜色相同的连续像素合并，打印 `(起点, 终点, 宽, 颜色)`：

```
$ python3 tools/measure.py assets/panel.png --runs 240
y=240 行色块游程：
  x  27- 114 宽  88 rgb(37,39,55)     ← 每核柱轨道
  x 116- 120 宽   5 rgb(9,15,26)      ← 间距
  …
  x 690- 777 宽  88 rgb(37,39,55)
```

期望：8 根柱 × 88px + 7 个 5px 间距（逻辑 60px 柱 / 4px 间距 × 1.48 ≈ 88/5.9）。

### 4.2 纵向节奏 —— 墨迹带（ink bands）

统计每行「非背景像素数」，超过阈值的连续行就是一条「墨迹带」（一行文字）：

```
$ python3 tools/measure.py assets/panel.png --bands
y 501- 528 高 28      ← 第 1 行进程
y 560- 586 高 27      ← 第 2 行（行距 57.6px ≈ 逻辑 39）
…共 10 条，间距均匀
```

期望：**行数 = `L_PROC_ROWS`**，间距均匀（不均匀 = 行高算错/越界）。

### 4.3 「某颜色画在哪」—— 按颜色 bbox + 逐行分布

```sh
# 用容差取色（抗抗锯齿），打印 bbox 与逐行 x 范围
magick shot.png -crop WxH+X+Y +repage -depth 8 rgb:- | python3 …
```

用途：确认某条曲线**画满了整段宽度**、某强调色**只出现在该出现的地方**。

⚠️ **别用「整屏扫某颜色」来定位局部**：别的栏里可能有同色元素（例如 peach 既在「↑ 速率」也在曲线里），
结果会误导。**先按区域 crop，再取色。**

---

## 五、判据四：性能（**每轮报数字**）

```sh
P=$(pgrep -x hpopi5b-panel)
read u1 s1 < <(awk '{print $14,$15}' /proc/$P/stat); sleep 20
read u2 s2 < <(awk '{print $14,$15}' /proc/$P/stat)
python3 -c "
hz=$(getconf CLK_TCK); d=($u2+$s2)-($u1+$s1)
print('%.2f%% of one core' % (d*1000.0/hz/20000*100))"
```

期望：**< 1%**（最早在 hphelm 上量到 0.60%）。

⛔ **这个数字在 hpopi5b 上量不出来，也别拿它当回归门槛**（2026-11-06 实测）：
Pi 的 cpufreq 是 `ondemand`，空载时 CPU 跑在 **408 MHz**（最低档），同样的绘制工作
按 CPU-秒算就贵 ~5 倍；机器忙（频率被顶到 1.8 GHz）时同一份二进制反而只有 2.9%。
⇒ **只能 A/B 比**：把旧二进制留在 `/usr/local/lib/hpopi5b-panel/hpopi5b-panel.prev`，
`echo performance > /sys/devices/system/cpu/cpufreq/policy0/scaling_governor` 固定频率后
先量旧版、再量新版（各 60 秒、留 15 秒预热），**同一个窗口里差值才有意义**。
2026-11-06 的实测（1.8 GHz 固定、真屏 `:0`）：旧三列 **3.47%** → 新单机 **3.68%**。
量完记得 `echo ondemand > …` 改回去。

内存：`grep -E '^(Pss|Pss_Anon)' /proc/$P/smaps_rollup` ⇒ Pss_Anon ≈ 5.4 MiB（PSS 总量会到 25 MiB，
那大部分是 X/picom/pango 的**共享库页**，不是我们的）。

### ⚠️ 单位陷阱（我自己犯过，错 10 倍）

`CLK_TCK = 100` ⇒ **1 jiffie = 10ms**。10 秒窗口的百分比 = `delta / 10`，**不是** `delta / 100`。
我第一版把「空转 33~49%」写成「3.3~4.9%」、把「0.60%」写成「0.08%」并已写进文档。
⇒ **落任何数字进文档前，先把单位和公式写出来对一遍**；百分比一律由「毫秒 / 窗口毫秒」现算。

---

## 六、判据五：红线没被碰（**每轮必跑，10 秒）**

```sh
cat /sys/class/tty/tty0/active     # hpopi5b = **tty1**（kiosk 的 xinit 就是 `Xorg :0 vt1 -keeptty`）；
                                  # hphelm 时代的 tty7 是历史值；greeter 抢 VT 时会是 tty8
pgrep -cx slick-greeter            # 0
pgrep -cx hpopi5b-panel             # 1（多了 ⇒ 单实例锁失效）
sha256sum /tmp/panelb/hpopi5b-panel /usr/local/lib/hpopi5b-panel/hpopi5b-panel  # 一致 ⇒ 装的是刚构建的那个
tail -5 /tmp/hpopi5b-panel.log      # 没有「关屏兜底」「连续 3 次」等兜底痕迹
```

---

## 七、开发时的隔离（**不要一上来动真屏**）

```sh
Xvfb :9 -screen 0 1920x1080x24 -ac &
DISPLAY=:9 ./hpopi5b-panel --state panel --lock /tmp/xv9.lock
DISPLAY=:9 import -window $(DISPLAY=:9 xdotool search --name hpopi5b-panel | tail -1) /tmp/shot.png
```

- **必须用 `--lock`** 换个锁路径：默认锁是全机 `/tmp` 的，否则 Xvfb 实例会因「已有实例」直接退出。
- ⚠️ **杀进程用 `pkill -x hpopi5b-panel` 会连真机实例一起杀**（同名）——
  真机上那台是守护循环托管的，杀了会被自动拉起、用户屏幕会闪一下。
  **测试时按 `DISPLAY` 挑 pid**：`grep -l 'DISPLAY=:9' /proc/*/environ` 或直接记住自己起的 pid。
- `Xvfb` 没有 GPU/picom，**观感类判断要在真机复核**（帧差分、性能、几何三类判据两边都有效）。

---

## 八、截图入库

```sh
# 真机或 Xvfb 抓 1920x1080 → 压成 PNG → 放 assets/（README 用）
magick shot-panel.png -strip -quality 90 assets/panel.png
```

截图只用于 README 展示；**判据不依赖截图**（判据是脚本输出的数字）。

---

## 九、双主机（生产服务器）判据（2026-10-21 新增）

```sh
# ① 链路通：--dump 会真的拉一帧（12 秒超时）
./hpopi5b-panel --dump | grep '^remote host='
# 期望：remote host=hpcore ncore=16 cpu=… mem=…/… disk=… temp=… dock=15 nproc=… ip=…

# ② 只在面板态跑：回桌面后子进程必须消失
pid=$(pgrep -x hpopi5b-panel); ps --ppid $pid -o cmd | grep ssh || echo "（桌面态无 ssh ✅）"

# ③ 断线可恢复：杀掉远端探针，列标题应在 5 秒内变「离线 Ns」，60 秒后变红
ssh -T hpcore 'pkill -f "perl -"'
sleep 6; DISPLAY=:0 import -window "$(xdotool search --name hpopi5b-panel|tail -1)" /tmp/off.png
python3 tools/render-region.py /tmp/off.png 1150 80 1408 140 4   # 看右上角状态文字
```

⚠️ **别用「杀远端进程」来验断线**：`ssh` 会立刻重连、2 秒内就恢复了，肉眼看不出来。
可靠的模拟是**暂停本地 ssh 子进程**：

```sh
spid=$(ps --ppid $(pgrep -x hpopi5b-panel) -o pid= | head -1)
kill -STOP $spid; sleep 7   # 数据停止到达 ⇒ 面板应显示「离线 Ns」
kill -CONT $spid            # 恢复 ⇒ 应立刻回到「在线」
```

## 十、量 CPU 的规矩（血泪，见 `.ai/pitfalls/cases/2026-10-21-*`）

```sh
uptime            # ⚠️ load > nproc/2 就别量了，量到的是别人的负载
pid=$(pgrep -x hpopi5b-panel)
a=$(awk '{print $14+$15}' /proc/$pid/stat); sleep 20
b=$(awk '{print $14+$15}' /proc/$pid/stat)
# CLK_TCK=100 ⇒ 1 jiffie = 10ms
python3 -c "print(f'{(($b-$a)*10)/20000*100:.2f}% of one core')"
```

**报数字时必须一起写**：窗口长度 · load average · 是否含 `--remote`。
优化前后的对比必须**在同一窗口内交替**（A10s B10s A10s B10s 取中位数），不许串行跑两轮。

## 十一、远程采集链路的回归判据（每次改 `remote.[ch]` 必跑）

这条链有**三种**失败形态，必须分别验，不能只验一种（2026-10-21 就只验了一种，漏掉两个真 bug）：

```sh
# ── 形态 1：正常连接
./hpopi5b-panel --state panel --remote hpcore
# 期望 <5s：# 拉起 ssh（第 1 次尝试） → 已连上（主机 hpcore，16 核，N 个进程）

# ── 形态 2：桌面 ↔ 面板 来回切（**最容易漏的一种**）
pid=$(pgrep -x hpopi5b-panel)
kill -USR1 $pid; sleep 3     # 进登录页 ⇒ ssh 子进程应消失
kill -USR2 $pid; sleep 8     # 回面板 ⇒ 应重新拉起并「已恢复」
# 判据：不得出现「无数据 ⇒ 掐掉重连」紧跟在同一秒（那是我踩过的「拉起→秒杀」死循环）
grep -c "无数据 ⇒ 掐掉" /tmp/hpopi5b-panel.log   # 期望 0

# ── 形态 3：连上了但永远没数据（对方探针没起来）
mkdir -p /tmp/fakebin && printf '#!/bin/sh\nsleep 3600\n' > /tmp/fakebin/ssh && chmod +x /tmp/fakebin/ssh
PATH=/tmp/fakebin:$PATH ./hpopi5b-panel --state panel --remote hpcore
# 期望 20s 内：「连接后 20s 无数据 ⇒ 掐掉重连」→「拉起 ssh（第 2 次尝试）」
rm -rf /tmp/fakebin
```

**三个坑，分别对应一种形态**（都不报错，只能靠日志）：

| 形态 | 症状 | 根因 |
| --- | --- | --- |
| 2 | 面板显示离线，日志疯狂「拉起→连续 269s 无数据→掐掉」 | 超时基准用了**历史**的 `last_frame`，不是本次连接的 |
| 3 | 面板永久离线，日志什么都没有 | 超时判据漏了 `saw_frame == 0` 这条路径 |
| 1 | — | （基线，用来对照前两种） |
