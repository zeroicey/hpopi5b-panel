# 部署：怎么把改完的代码变成「系统屏保」

> **这是本项目最重要的一份操作文档。** 它回答一件事：
> **改完代码，怎样安全地把它变成此刻正在工位屏幕上跑的那个东西，并且知道它真的好了。**
>
> 适用机：`hphelm`（GMKtec G10，别名就是 `hphelm`）· 最后实测：**2026-10-21**（三条判据链全绿，
> 只有 CPU 一条是黄字，见第七节末尾的「已知偏差」）。
> 相关：[`architecture.md`](architecture.md) · [`verify.md`](verify.md) · [`pitfalls.md`](pitfalls.md) ·
> [`../AGENTS.md`](../AGENTS.md) · 机队视角的权威在 `kb/runbooks/hphelm.md`

---

## 零、30 秒版（**现役：kiosk on hpopi5b；源码在 hpcore，2026-11-06 起**）

**hpcmd 已于 2026-11-06 下线**（全机队 all-in-one 到 hpcore）。现在的两段式：

| 角色 | 机器 | 路径 |
| --- | --- | --- |
| 源码工作副本（改这里、提交、push） | **hpcore**（`192.168.5.102`，本机） | `/home/happpy/work/project/hpopi5b-panel` |
| 构建 + 运行（arm64 kiosk） | **hpopi5b**（`192.168.5.106`） | 构建 `/root/hpopi5b-panel` · 二进制 `/usr/local/lib/hpopi5b-panel/` |

⚠️ 两边都**不要**跑 `tools/deploy.sh`（它是 hphelm 时代「本机原地七阶段部署」，落点已不存在）。
⚠️ `hpcore` 上没有 gcc/make/pkg-config ⇒ **构建只能在 hpopi5b 上做**；`hpcore` 上有 `gh` 登录态（`zeroicey`）
⇒ 提交与 `git push` 在本机做即可。

```sh
# ① hpcore（本机）：改 → 提交 → push
cd /home/happpy/work/project/hpopi5b-panel
git add -A && git commit -m "..." && git push origin main

# ② 同步到 hpopi5b 并 arm64 本地编译（`make panel`，不要 `make`）
tar --exclude=.git --exclude='*.o' -czf /tmp/panel.tgz . && scp -q /tmp/panel.tgz hpopi5b:/tmp/
ssh hpopi5b 'rm -rf /root/hpopi5b-panel && mkdir -p /root/hpopi5b-panel && \
  tar -xzf /tmp/panel.tgz -C /root/hpopi5b-panel && cd /root/hpopi5b-panel && make panel'
#   ⚠️ `make` 会连带构建 setuid PAM 助手（hpopi5b-auth），kiosk 不需要 ⇒ 用 `make panel`

# ③ 安装 + 重启 + 判据
ssh hpopi5b 'install -m755 /root/hpopi5b-panel/hpopi5b-panel /usr/local/lib/hpopi5b-panel/hpopi5b-panel && \
  install -m644 /root/hpopi5b-panel/tools/probe-remote.pl /usr/local/share/hpopi5b-panel/probe-remote.pl && \
  systemctl restart hpopi5b-kiosk'

# ③b 只有「新闻幕 / 趋势幕」的抓取器变了才需要这几行（首次装 unit 与定时器）
ssh hpopi5b 'install -m755 /root/hpopi5b-panel/tools/fetch-news.py /usr/local/lib/hpopi5b-panel/fetch-news.py && \
  install -m644 /root/hpopi5b-panel/systemd/hpopi5b-news.service /etc/systemd/system/ && \
  install -m644 /root/hpopi5b-panel/systemd/hpopi5b-news.timer   /etc/systemd/system/ && \
  systemctl daemon-reload && systemctl enable --now hpopi5b-news.timer'
#   验证：`systemctl list-timers hpopi5b-news.timer` 有 NEXT（30min 一轮）·
#          `head -1 /var/cache/hpopi5b-panel/news.tsv` ⇒ `# news <时间> hot=N tech=M` 且 N/M 非 0
#   ⚠️ 抓取器是 python3 脚本，**不跟二进制同版本**：改了它就得单独 install（不会跟着 make 走）
```

判据（数字，别只看「像是对的」）：`systemctl is-active hpopi5b-kiosk` = active ·
`pgrep -cx hpopi5b-panel` = 1 · `DISPLAY=:0 xrandr --query` 有 `DP-1 ... 1920x1080` 且带 `*` ·
`DISPLAY=:0 tools/probe-frames 40 20` ⇒ `>30% 变化帧 = 0`（**kiosk 每 30s 换幕的那 2 帧除外**，见 `verify.md` §二）· 日志无 X Error。
完整 9 条判据与坑档见 **`kb/runbooks/hphelm-panel.md` §四**（该 runbook 随项目改名另行归档）。

> **历史（2026-10-21，hphelm 时代，已失效）**：`tools/deploy.sh` 是「本机原地七阶段部署」，落点
> `/usr/local/bin` / lightdm / 锁屏模式。**在 hpopi5b 上不要跑它**；它指向的机器与形态都已不存在。

## 一、先说清「屏保」是什么（不然会找错东西）

这台机上**没有** `xscreensaver` / `light-locker` / `dm-tool`。所谓的「屏保」就是这个项目自己：

| 状态 | 屏幕上看到什么 | 谁在画 |
| --- | --- | --- |
| `PANEL` | 运维看板（CPU/内存/网络/磁盘/每核柱/进程榜 24 条/时钟），**7×24 常亮** | `hpopi5b-panel --state panel` |
| `LOGIN` | 动键盘/鼠标 ⇒ 自绘登录页（PAM 认证） | 同一个进程，切状态 |
| 桌面 | 认证通过 ⇒ 面板取消映射，露出 bspwm 桌面 | 面板让位 |
| 再空闲 180s | 面板覆盖回来 | 同上 |

所以：

- **「更新系统屏保」= 用新的 `/usr/local/bin/hpopi5b-panel` 替换旧的，并让它重新跑起来。** 就这一件事。

### 被监控机（可选，默认 `hpcore`）

面板画一台远程主机（默认 `hpcore`）；**没配就画本机**。配置只有一个文件：

```sh
sudo tee /etc/hpopi5b-panel.conf <<'EOF'
# 被监控机：ssh 目标名（~/.ssh/config 里的 Host 别名）。留空/删掉这行 = 只显示本机。
remote=hpcore
# remote2= 自 2026-11-06 起废弃（hpcmd 下线）：写了也不生效，只打一行提示。
EOF
```

优先级：`--remote <host>` > `$HPOPI5B_REMOTE` > `/etc/hpopi5b-panel.conf`。
**改完 `pkill -x hpopi5b-panel`**（守护循环会用新配置拉起）。

⚠️ 前提：本机到该目标**免密 ssh 可用**（`ssh -T -o BatchMode=yes hpcore true`），
且对方有 `perl`（Debian/Arch 都自带，**不需要装任何东西**）。
链路自检：`./hpopi5b-panel --dump | grep '^remote host='`。
- 它由 **lightdm 的会话入口 `/usr/local/bin/hphelm-session`** 拉起（在 `while :; do … done` 守护循环里），
  ⇒ `pkill -x hpopi5b-panel` 之后约 1 秒会自己用**新二进制**回来。**不需要重启 lightdm，也不需要退出登录。**
- 它**替代了 lightdm 的 greeter** ⇒ 它挂了就是「用户看不到登录页」⇒ 所以下面有三条红线。

---

## 二、机器上到底有哪些文件（谁装、谁管）

| 路径 | 是什么 | 谁负责写 |
| --- | --- | --- |
| `/data/dev/hpopi5b-panel/` | **源码 = 部署目录**（同一个目录，git 仓，`main` 分支） | 你 |
| `/usr/local/bin/hpopi5b-panel` | 主程序（~71 KB；`ldd` 里 GUI 工具包 **0** 个） | `sudo make install` |
| `/usr/local/libexec/hpopi5b-auth` | setuid 认证助手，**必须是 `4755 root:root`** | `sudo make install` |
| `/usr/local/share/hpopi5b-panel/probe-remote.pl` | 生产服务器探针（**644，不是可执行文件**；由 ssh 喂到对方 stdin） | `sudo make install` |
| `/etc/hpopi5b-panel.conf` | `remote=<ssh 目标>`（改主机名只改这里，不用动会话包装器） | **手工创建**（见下） |
| `/usr/local/bin/hpopi5b-panel.prev` | **上一版二进制**（deploy.sh 在内容不同时自动留） | `deploy.sh` |
| `/usr/local/bin/hphelm-session` | lightdm 会话入口：`xset`、日志轮转、守护循环、兜底 | **手工，一次性**（见第五节） |
| `/etc/pam.d/hpopi5b-auth` | **专用** PAM 服务（只留 `pam_unix`） | **手工，一次性**（见第五节） |
| `/usr/local/sbin/hphelm-vt-guard` + `hphelm-vt-guard.service` | 独立防线：被抢的 VT 拽回 tty7 | **手工，一次性**（见第五节） |
| `/tmp/hpopi5b-panel.log` | 运行日志（会话启动时 >1 MiB 就轮转） | 运行时自动 |
| `/data/backups/hpopi5b-panel-20261018/` | 整机级归档（git 历史 + 四处系统配置 + `RESTORE.md`） | 别删 |

**读这张表的方式**：只有前两行是「你每天会动的」；其余是「装了一次就基本不动」的系统文件，
动它们之前先读第五节和第八节。

---

## 三、日常开发循环（改代码之前先看这个）

```sh
# ① 构建
make

# ② 在隔离的 X 里看效果 —— 不要一上来动真屏
Xvfb :9 -screen 0 1920x1080x24 -ac &
DISPLAY=:9 ./hpopi5b-panel --state panel --lock /tmp/xv9.lock   # ⚠️ --lock 必须换，否则和真机实例抢锁
DISPLAY=:9 import -window "$(DISPLAY=:9 xdotool search --name hpopi5b-panel | tail -1)" /tmp/p.png
python3 tools/render-ascii.py /tmp/p.png 22 22                 # 粗看版式
python3 tools/measure.py /tmp/p.png --runs 240 --bands         # 量几何

# ③ 看不到图也要能量（这是本项目的核心方法）
#    见 docs/verify.md：帧差分判闪 / 几何判版式 / 仪表判空转 / /proc 判性能
```

⚠️ **在 Xvfb 里测的时候不要 `pkill -x hpopi5b-panel`** —— 那会**连真机实例一起杀**（同名）。
按 pid 挑（认 `/proc/<pid>/environ` 里的 `DISPLAY`），或者用完 `kill <pid>` 指定 pid。

⚠️ **`kill -USR2` 只切状态，不会重载二进制**：换了代码就必须重启进程。

---

## 四、部署（工具做的事，逐条可核对）

`tools/deploy.sh` 有七个阶段，每一阶段都会打一行 `==> n/7`。`--check` 只打印、不落地。

| 阶段 | 做什么 | 失败会怎样 |
| --- | --- | --- |
| 1/7 环境自检 | 源码目录、`pkg-config` 六个包、`make`、免密 `sudo`、VT=tty7、lightdm 在跑 | 直接退出，不动系统 |
| 2/7 构建 | `make`；**要求 0 警告**（有警告只黄字提醒）；打印字节数与 `ldd` 里 GUI 工具包数量（期望 0） | 构建失败 ⇒ 原样保留旧二进制，屏幕不受影响 |
| 3/7 备份 | 新构建与已装**内容不同**时，`cp -a` 到 `/usr/local/bin/hpopi5b-panel.prev` | 这一步是回滚的凭据 |
| 4/7 安装 | `sudo make install`；随后校验「源码 ⇄ 已装」逐字节一致、助手权限 `4755 root:root` | 任一不符 ⇒ 红字退出 |
| 5/7 语义自检 | `./hpopi5b-panel --dump \| tail -1`（**不需要 X，不抢单实例锁**），断言含 `sleep_off=off` | 不符 ⇒ 停下（说明默认值或判据被改回去了，`docs/pitfalls.md` #16） |
| 6/7 重启 | `pkill -x hpopi5b-panel`，等 ≤15 秒让守护循环用新二进制拉起，并确认 **pid 变了** | 15 秒没起来 ⇒ 守护循环没在跑，看日志 |
| 7/7 判据 | VT / greeter / 单实例 / 帧差分 / CPU / 当前实例日志 | 红线失败 ⇒ exit 1 |

**手动等价（脚本坏了才用，别养成习惯）**：

```sh
cd /data/dev/hpopi5b-panel
make && sudo make install                       # 构建 + 安装
./hpopi5b-panel --dump | tail -1                 # 语义自检：应含 sleep_off=off
pkill -x hpopi5b-panel                           # 守护循环 ~1s 后用新二进制拉起
```

⚠️ `pkill -x` **不是** `pkill -f`：`-f` 会匹配到执行它的那个 shell 自己（我们踩过四次）。
⚠️ 重启会让桌面露出 **1~2 秒**（面板 unmapped），这是正常的，不要为了「无感更新」去改架构。

---

## 五、只做一次的三处系统配置（重建机器时才需要）

### 5.1 会话入口 `/usr/local/bin/hphelm-session` 的三段关键代码

```sh
# ⛔ 永不出现 dm-tool / light-locker
xset s off
xset dpms 0 0 0          # DPMS 定时交给面板自己管（默认 sleep_off=0 ⇒ 永不关屏）

# 日志轮转：>1MiB 就 tail 到 256K（直接 rm 会删掉正在写的 inode，日志从此「看起来是空的」）
[ -f /tmp/hpopi5b-panel.log ] && [ "$(stat -c %s /tmp/hpopi5b-panel.log)" -gt 1048576 ] && \
  { tail -c 262144 /tmp/hpopi5b-panel.log > /tmp/hpopi5b-panel.log.1; mv -f /tmp/hpopi5b-panel.log.1 /tmp/hpopi5b-panel.log; }

(
  fails=0
  while :; do
    start=$(date +%s)
    /usr/local/bin/hpopi5b-panel --state panel >>/tmp/hpopi5b-panel.log 2>&1
    rc=$?; dur=$(( $(date +%s) - start ))
    printf '[%s] hpopi5b-panel 退出 rc=%s 运行 %ss\n' "$(date +%T)" "$rc" "$dur" >>/tmp/hpopi5b-panel.log
    [ "$rc" -eq 3 ] && { sleep 30; continue; }        # 已有实例（单实例锁）
    [ "$dur" -lt 5 ] && fails=$((fails+1)) || fails=0 # ⚠️ 判据是「运行时长」，不是退出码
    if [ "$fails" -ge 3 ]; then
      printf '[%s] 连续 3 次「起来就死」⇒ 关屏兜底（不碰 lightdm）\n' "$(date +%T)" >>/tmp/hpopi5b-panel.log
      xset dpms force off >/dev/null 2>&1 || true     # ⛔ 绝不 dm-tool lock
      sleep 30; fails=0
    fi
    sleep 1
  done
) &
```

### 5.2 PAM：`/etc/pam.d/hpopi5b-auth` 只留 `pam_unix`

```sh
cat >/etc/pam.d/hpopi5b-auth <<'EOF'
auth     required pam_unix.so
account  required pam_unix.so
EOF
```

⛔ **绝不要写成 `@include login` / `system-local-login`**：那条链带 `pam_faillock`，而
`lightdm-autologin` 第一行也是 `pam_faillock.so preauth` ⇒ 锁屏输错 3 次会把账号锁上、
连坐 autologin 失败 ⇒ lightdm 起 greeter 抢走 VT（**用户就会看到那个本该消失的旧登录页**）。

### 5.3 VT 守卫

| 项 | 值 |
| --- | --- |
| 脚本 / 单元 | `/usr/local/sbin/hphelm-vt-guard` · `/etc/systemd/system/hphelm-vt-guard.service`（enabled） |
| 动作 | 活跃 VT ≥ 8 **且** `slick-greeter` 在跑 **且** `bspwm` 在跑 ⇒ `chvt 7` |
| 边界为什么这么窄 | 不干扰 `Ctrl+Alt+F2` 救援 TTY（tty2~tty6 永不介入） |
| 判据 | `sudo chvt 2; sleep 3; cat /sys/class/tty/tty0/active` ⇒ 仍 `tty2` |

---

## 六、判据表（**部署完必须逐条对上**）

| # | 命令 | 期望 | 不符意味着什么 |
| --- | --- | --- | --- |
| 1 | `cat /sys/class/tty/tty0/active` | `tty7` | 有东西抢了 VT（`tty8` = greeter 起来了 ⇒ 红线 1 被碰） |
| 2 | `pgrep -cx slick-greeter` | `0` | ≠0 ⇒ 面板没在当锁屏，用户在看着 DM 的旧登录页 |
| 3 | `pgrep -cx hpopi5b-panel` | `1` | `0` 守护循环死了；`2` 单实例锁失效（会互相抢抬窗） |
| 4 | `md5sum ./hpopi5b-panel /usr/local/bin/hpopi5b-panel` | 一致 | 装错版本（或装到别处） |
| 5 | `./hpopi5b-panel --dump \| tail -1` | 含 `sleep_off=off` | 默认值/判据被改回去了（#16） |
| 6 | `DISPLAY=:0 tools/probe-frames 40` | `>30% 变化帧 = 0`（换幕帧除外） | 有闪 ⇒ 双缓冲被绕过了 |
| 7 | CPU（`/proc/<pid>/stat` 的 14/15 字段，20 秒窗口） | < 1% of one core | 见第七节末尾「已知偏差」 |
| 8 | `tail -3 /tmp/hpopi5b-panel.log` | `rc=143` = 我们主动重启（正常）；**不能出现**当前实例的「连续 3 次」 | 出现 ⇒ 面板在反复崩，兜底已经关屏 |

⚠️ **第 8 条的坑**：`/tmp/hpopi5b-panel.log` 是**跨会话追加**的，2026-10-18 那两次事故的
「连续 3 次异常退出 ⇒ 退回 dm-tool lock 兜底」还在文件里 ⇒ **对全文件 grep 会假告警**
（我写 `deploy.sh` 第一版就这么误报过）。正确做法：只取**最近一条 `启动：` 之后**的日志块。
`deploy.sh` 里已经这么实现了。

### 已知偏差（2026-10-21 实测，留给项目 AI 查）

| 项 | 记录值 | 2026-10-21 实测 | 备注 |
| --- | --- | --- | --- |
| CPU | 0.60% of one core（2026-10-18） | **1.13%–1.60%**（20/30 秒窗口，多次） | 不闪、不空转、几何都对；**机理未定**。查法：`HPOPI5B_PANEL_DEBUG=1` 看 it/s 与各阶段耗时，别先猜 |

---

## 七、回滚（三个层级，从轻到重）

### A. 只是新版不好 ⇒ 换回上一版二进制（最常见，10 秒）

```sh
sudo install -m755 /usr/local/bin/hpopi5b-panel.prev /usr/local/bin/hpopi5b-panel
pkill -x hpopi5b-panel        # 守护循环会把它拉起来
```

`deploy.sh` 在「新构建 ≠ 已装」时会自动写 `.prev`。**注意**：它只保留**上一版**。
想留更多版本就自己 `cp -a /usr/local/bin/hpopi5b-panel /data/backups/panel-$(date +%F-%H%M)`。

### B. 会话入口 / 快捷键被改坏了 ⇒ 恢复系统文件

```sh
sudo systemctl disable --now hphelm-vt-guard.service        # 先停守卫，否则它一直把 VT 拽回 tty7
sudo cp -a /usr/local/bin/hphelm-session.bak-prepanel /usr/local/bin/hphelm-session
cp -a ~/.config/sxhkd/sxhkdrc.bak-prepanel ~/.config/sxhkd/sxhkdrc
pkill -x hpopi5b-panel
sudo systemctl restart lightdm                              # ⚠️ 会杀掉当前图形会话
```

### C. 完全回到「lightdm greeter 当锁屏」（放弃自研面板）

```sh
sudo systemctl disable --now hphelm-vt-guard.service
sudo /usr/local/bin/hphelm-lockd >/dev/null 2>&1 &   # v1 的空闲锁屏守护（二进制可能已不在，见下）
pkill -x hpopi5b-panel
sudo systemctl restart lightdm
```

⚠️ `hphelm-lockd` 是 **v1** 留下的脚本，`/usr/local/bin/` 里现在**可能没有**（它随 v1 退休）。
真要走 C 层，改用 lightdm 自带锁（`light-locker`）**会重新引入「登录两遍」的两把锁 bug** ——
先读 `docs/pitfalls.md` #15 再决定。

### 整机级归档

`/data/backups/hpopi5b-panel-20261018/`：`panel2.bundle`（完整 git 历史）+ 四处系统配置 + `RESTORE.md`。

---

## 八、⛔ 绝对不要做的六件事

1. **不给 lightdm 发任何锁请求**（`dm-tool lock` / `light-locker` / 让 autologin 失败）⇒ 会起 greeter 抢 VT，用户看到旧登录页。红线 1。
2. **不用退出码判守护循环失败**（SIGTERM ⇒ 143）⇒ 用「运行时长 <5 秒」。否则每次调试重启都在攒失败计数。红线 2。
3. **回滚/改会话入口前不停 VT 守卫** ⇒ 它会把 VT 一直拽回 tty7，让你误判「改了没生效」。红线 3。
4. **不用 `pkill -f <pattern>`** ⇒ 会匹配到执行它的 shell 自己（踩过四次）。用 `pkill -x` 或写进脚本文件。
5. **不 `rm -f /tmp/hpopi5b-panel.log`** ⇒ 进程仍持 fd，继续往已删除 inode 写：日志「看起来空了」、空间还不释放。要清就轮转（`tail -c` + `mv`）。
6. **不 `systemctl restart lightdm` 来「试一下」** ⇒ 那会杀掉用户正在用的图形会话。

---

## 九、交付清单（一轮改动的「完成」定义）

- [ ] `make` 0 警告；`deploy.sh` 判据全绿（黄字提醒要在 `HANDOFF.md` 记一笔）
- [ ] 改的是 UI ⇒ 在 Xvfb 里量过几何（柱宽/间距/对齐/行距），与设计值逐位对过
- [ ] 改的是主循环/渲染 ⇒ 跑过帧差分（0 帧 >30%）与仪表（2 it/s、poll 全超时、事件 0）
- [ ] `HANDOFF.md` 追加本轮小结（做了什么 / 判据数字 / 踩了什么）
- [ ] 行为、版式、接口有变 ⇒ 同步 `README.md` 与对应 `docs/*.md`
- [ ] `.ai/CURRENT_TASK.md` 更新；新决策 → `.ai/decisions/`；新坑 → `.ai/pitfalls/cases/`
- [ ] **机队事实有变**（部署路径、服务名、红线、PAM、VT 守卫）⇒ 提醒维护 kb 的人更新
      `kb/runbooks/hphelm.md` 的「运维面板（hpopi5b-panel）」节
- [ ] `git commit`（`main` 分支直接提交，不 force-push、不改历史）

---

## 十、历史说明（为什么文档里还留着 hpcore / rsync）

2026-10-18～10-19 期间，源码权威在 `hpcore:/data/dev/hpopi5b-panel`，部署靠
`tools/deploy.sh` 用 rsync 推到 `hphelm:~/projects/panel2`（后来改成 `hphelm:/data/dev/hpopi5b-panel`），
再 `ssh hphelm 'make && sudo make install'`。

**2026-10-19 起整仓迁到 hphelm，源码与部署目录合一** ⇒ rsync 与「ssh 自己连自己」都不需要了。
那条老路现在**必然失败**：hphelm 的 `~/.ssh/config` 里没有 `hphelm` 这个 Host，
`ssh hphelm` 直接 `Host key verification failed`。⇒ `tools/deploy.sh` 已改成**原地部署**（2026-10-21）。
