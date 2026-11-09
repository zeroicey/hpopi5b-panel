# 交接：hpopi5b-panel（运维面板 + 自研锁屏）

> **本轮状态（第十八轮：新闻幕两列 → 三列 + 首条放大，2026-11-09 傍晚）**：用户口径「感觉看起来的效果一般般很单调
> 空间利用率不够 国内热点和科技这一行标题可以删掉了」⇒ 给了三个方案，用户选「三列 + 首条放大」。
> 新闻幕从**两列各 15 条（装 29 条、行距空荡）**改成 **1+2 列 · 两行式 44px · 首条 92px（装 41 条）**，
> 删掉列标题行，分组靠「列宽 34% / 66% + 两条竖细线 + 首条放大」；热点列第二行是量度杆（左端钉死、长度封顶 150），
> 科技列第二行是「来源 / 相对时间」。顺手挖掉一个真性能坑：`content_sig()` 把 1 Hz 采样快照无条件混进签名，
> 导致**只吃外部文件的幕也每秒整屏重绘** ⇒ 加 `scene_t.live`，真屏 CPU 反而从 **2.94% 降到 2.01%**。
>
> **前一轮（第十七轮：新闻幕，2026-11-08 晚）**：用户口径「想加一幕，关于新闻的 但是我们的数据源选哪里呢 要哪种新闻呢」＋
> 三问三答（形式「两列吧」/ 频率 30 分钟 / 左国内热点右科技）⇒ 新增 `news` 幕：左「国内热点」14 条带热度，
> 右「科技」15 条带「来源 · 10h」。数据由 `tools/fetch-news.py`（头条热榜，退化百度热搜）＋ 三路科技 RSS 抓取，
> `hpopi5b-news.timer` 每 30 分钟喂缓存 TSV，幕**只读文件**。选源是在 hpopi5b 上逐条实测的（微博/知乎是登录墙，
> 人民网/新浪 RSS 是僵尸），结果写进 `docs/architecture.md`「news 幕的数据通路」表。
>
> **前一轮（第十六轮：GitHub 趋势幕填满整屏，2026-11-06 夜）**：用户口径「GitHub 趋势那一幕我感觉还是有很多空白空间
> 内容还能加多 你利用一下」⇒ 趋势幕从 **2 列 × 8 格固定网格**（丢 5 条、下部 ~55 逻辑像素空着）改成
> **流式两列**：行高按描述实际 1~2 行算、两列零头均摊进行距（≤8 像素/行）、15 条全上屏、
> 右列尾部一行语言分布脚注。过程中挖出两个坑（行尾 `\n` 让每条多算一行 24 像素；1 Hz 重画下每帧重排
> 白烧 1.5% CPU）——都已修并写进 `docs/pitfalls.md` §22/§23。
>
> **前一轮（第十五轮：容器块 → Tailscale 尾网块，2026-11-06 傍晚）**：第十四轮上线的容器清单被用户否掉
> （「不对啊 容器好像不就是进程吗 那算了 不要了 换成别的吧 你看看可以显示 tailscale 的相关东西 看看怎么做比较好」）。
> ⇒ 同一块位置换成 **Tailscale 尾网**：两个子列各 5 台，名字 · 系统 · `直连`/`中继 <DERP区域>` ·
> 此刻收发速率（离线设备「离线 · Nh 前」）；标题行给「9 台 · 7 在线」「本机 100.64.0.1」「直连 4 · 中继 2」。
> 数据来自 **tailscaled 的 localapi**（unix socket 读 JSON），**不 fork `tailscale status`**、不需要 root。
> 容器那套（`D` 行 / `docker.sock` / `ctr_*` / `draw_ctr_block` / `L_CTR_*`）**全部删除**，
> 只留下「容器 N」这个计数（cgroup 目录数，不碰 socket）。
>
> **更前**：第十四轮一条进程榜 + 容器块（容器块**已被取代**）· 第十三轮单机化（`remote2` 废弃）·
> 第七轮双主机三列（**已被取代**）· 第六轮部署链本土化 · 第十二轮第 4 格曲线换磁盘 I/O ·
> 更早：桌面↔面板↔登录跑通 → 消闪（离屏 pixmap）→ 修自激空转 → 无卡片紧凑版式。
>
> **最后更新：2026-11-09（第十八轮：新闻幕三列化 + `scene_t.live` 修掉每秒重绘）。**

---

## 第十八轮：新闻幕两列 → 三列 + 首条放大（2026-11-09）

**用户口径**（原话）：「看见你加的了 但是感觉看起来的效果一般般很单调 空间利用率不够  国内热点和科技这一行
标题可以删掉了 你看看有什么好的方案」。用 `ask_user_question` 给了三个方案（各带 ASCII 预览）：
A 三列 + 首条放大 / B 两列压紧 + 纹理 / C 四列高密度 —— **用户选 A**。

### 改了什么

| 文件 | 改动 |
| --- | --- |
| `src/scene_news.c` | 重排 `draw()`：先算条数（顶栏要报**真的排上了几条**）→ 顶栏 → 空态 → `plan()` → 两条竖细线 → `draw_hot()` + `draw_tech()×2`。新增 `v_text_wrap` 首条、`heat_bar()`（轨道 + 游标两段矩形）、`heat_of()`（`1337万`/`1.2亿` → 数值）、`metric()`（杆 + 右对齐数值，杆左端钉死 `x+L_NW_RANK_W`）。`plan()` 缓存键加 `hotcw/techcw`，拆出 `p_tech_title/src/age`。`SCENE_NEWS` 加 `live=0` |
| `src/theme.h` | 删 `L_NW_HEAD_H` / `L_NW_RIGHT_W` / `L_NW_ROW_PAD_MAX`；加 `L_NW_LEAD_H 92`、`L_NW_AGE_W 44`、`L_NW_HOT_FRAC 0.34`、`L_NW_BAR_H 2.5`、`L_NW_BAR_W 150`、`L_NW_BAR_W_LEAD 210`、`L_NW_BAR_GAP 12`、`L_NW_COL_PAD_MAX 8`；`L_NW_Y0 = L_BODY_Y+6`、`L_NW_ROW_H 44` |
| `src/scene.h` | `scene_t` 加 `int live`（1 = 画面随 1 Hz 采样变） |
| `src/panel.c` | `content_sig()`：`M` / `R.m` 只在 `state != ST_PANEL || sc->live || scenes_transitioning()` 时混；`FEED.age_s` 只在**离线**时混；`sc->sig` 判空 |
| `src/scene_panel.c` / `src/scene_trending.c` | `live=1` / `live=0` |
| `tools/fetch-news.py` | 抓取上限 `--hot 14/--tech 30` → **20/32**（多抓当余量：上屏条数由版式算，现在是 13 + 28） |

### 判据数字

- 几何（`LAYOUT_DEBUG=1`，`:9 --scale 1.48`）：`news: hot=13/20 tech=28/30 hotw=411.2 techw=399.1 y0=64 avail=639.7
  rows_hot=12 rows_tech=14 lead=92 row=44 H_ROW=24` ⇒ 上屏 **41 条**（旧版 29 条）。
- 真屏 `:0`：`probe-frames 60` ⇒ **>30% 像素变化的帧 = 0**；`tty1` · `slick-greeter=0` · `hpopi5b-panel=1`。
  三幕（panel / trending / news）自动轮换都抓到了，第二张真屏截图与 `:9` 渲染逐像素一致。
- **CPU（1.8 GHz `performance`，90 s 窗口，3 幕轮换，同参数 A/B）**：
  **旧二进制（`.prev`，两列新闻幕 + 每秒重绘）2.94% → 新 2.01%**。内容多 40%，净省 0.93 个百分点。
- 单帧绘制 `绘制`：news 幕 ~20 ms（旧 6 ms）—— 但只在分钟翻转 / 缓存改写时发生，见下面的坑。

### 踩了什么

1. **重写 `fit()` 时丢了 `if (mid == lo) break;`** ⇒ UTF-8 回退到 `lo` 仍「够窄」的话死循环，面板**卡在 `win_show()` / 画不出窗口**（截图只有 root 背景）。二分函数重写必须与上游 `git show HEAD:src/` 逐行对齐；这类 bug 编译零告警。
2. `L_NW_ROW_H 40` 装不下「13pt 行高实测 **24**（不是 18）+ 9pt 16.6」⇒ 刻度杆压在字形下沿。
   **行高要用 `v_text_h()` 量完再加**，不要拿字号乘系数赌。
3. 刻度杆一开始铺满整列（≈329px）像「高亮下划线地毯」；改成右对齐到数字又会让杆从标题中段长出来。
   ⇒ 正解是 **左端钉死 + 长度封顶**（所有杆共用基线，长度才可比）。
4. **真性能坑**：`content_sig()` 无条件混 `M`（1 Hz 采样）与 `FEED.age_s`（在线时每秒在涨）
   ⇒ 新闻/趋势幕**内容没变也每秒整屏重绘**（20 ms/s ≈ 2%）。修法：`scene_t.live` + 离线才混 `age_s`。
   已写进 `docs/pitfalls.md` §24 与 `.ai/pitfalls/cases/2026-11-09-sig-mixes-live-metrics.md`。
5. 取证陷阱：本地 `/tmp/real2.png` 之类**重名旧文件**会让人以为真屏还在跑旧二进制。
   判断「真机到底在跑哪一版」要 `md5sum` 比二进制，不要靠截图文件名。

### 下一轮的候选

- 热点列首条与第二行之间有时能看到一点空（首条块固定 92，标题只有 1 行时度量组下沉 8px）——
  可改成「按实际行数收紧，但块高不变，留白给底部」。
- 科技 28 条铺两列后，第二列的 `1d/2d/3d` 比第一列的 `1h/2h` 旧，右重左轻是**数据顺序**决定的，不是排版错；
  若想更均衡可按时间切两半（简单）或蛇形排（复杂）。
- 36氪 feed 仍解析 0 条；`docs/verify.md` 的 python 工具链（依赖 ImageMagick）在 hpopi5b 上仍不可用。

---

## 第十七轮补丁：trending 只显示 10 行不是 bug（2026-11-09）

**用户口径**（m00001）：「现在的 github 的趋势榜好像有点问题 是本身就只获取到了 10 个吗」

### 结论：GitHub 自己只给了 9 条

| 取证 | 结果 |
| --- | --- |
| hpopi5b 直连 `github.com/trending`（HTTP/2 200、`server: github.com`、无代理头） | 完整页面（`</html>` 结尾、517 KB），`<article class="Box-row">` **9** 个、`stars today` **9** 处 |
| 另一条完全不同的出口（国外）抓同页 | 同样 9 个仓库、同样顺序 |
| 缓存 `/var/cache/…/trending.tsv` | 头行 `… 9` ⇒ 加表头共 **10 行** |
| 14:57 定时 + 手工再跑两次 | 都 9 条，稳定 |

⇒ 面板侧没有任何 10 条上限（`src/scene_trending.c:19` 的 `TR_MAX 24` 只是缓存上限），
**是 GitHub 当日榜就只有 9 个**；页里也没有 load-more（`hx-get` 出现 0 次）。

### 顺手补的真漏洞：半页会被静默写进缓存

原来的 `tools/fetch-trending.py` 只有 `if not rows` 一道下限。`IncompleteRead` 会崩（正确，systemd 保留旧缓存），
但「页面在条目边界断开」这种半页会被**无声无息写成短榜单**。现在（`tools/fetch-trending.py`）：

1. 结尾不是 `</html>` ⇒ 可疑；
2. `article` 条数与页面里 `stars today` 处数不一致 ⇒ 可疑；
3. 条数 < `max(5, 上一份 × 0.6)` ⇒ 可疑；

可疑就**重抓一次**（隔 5s）；两次都可疑 ⇒ **不覆盖缓存 + 非 0 退出**（journal 里有原因）；
两次一致 ⇒ 当真（否则「GitHub 真给 9 条」会把旧榜单永久钉住）。

**判据**（离线自测 7 例全过，monkeypatch `fetch_page` 不打网络）：正常 3 条接受 / 截断两次拒绝且缓存不变 /
首次截断重抓完整接受 / 条数对不上拒绝 / 网络异常拒绝 / 25→9 两次一致接受 / 25→9 后重抓 22 接受。
真机：`systemctl start hpopi5b-trending.service` 9 条一行退出码 0；造一份 25 条的假缓存再跑 ⇒ 隔 5s 重抓、
11s 后接受 9 条，journal 记为「trending: 9 条 → …」。

---

## 第十七轮：新闻幕（国内热点 + 科技）（2026-11-08 晚）

**用户口径**（m00001）：「想加一幕，关于新闻的 但是我们的数据源选哪里呢 要哪种新闻呢」
→ 我按惯例先问三件事（形式 / 频率 / 两列怎么分），用户答：**两列**、**30 分钟**、「左国内热点 / 右科技」。

### 数据源怎么选的（实测，不是查文档）

用 `curl` 在 hpopi5b 上逐条打（`eth0 → 192.168.5.1`，无 exit node）：

| 候选 | 结果 | 结论 |
| --- | --- | --- |
| 头条热榜 `hot-event/hot-board` | 200 / 0.46s / 50 条 / 带热度 | **左列首选** |
| 百度热搜 `top.baidu.com/api/board` | 200 / 51 条 / 带热度 | 退化方案（结构嵌套两层 `content[0].content[]`） |
| Solidot / 少数派 | 200（Solidot 裸抓 403，要 UA） | **右列主力** |
| 36氪 `36kr.com/feed` | 200 但**解析 0 条** | 留着待修（日志会打「跳过 36氪: 0 条」） |
| chinanews / 界面 RSS | 200 / 30 条 / pubDate 新鲜 | 备选：想「只挑时事」就用它换热搜 |
| 微博热搜 / 知乎热榜 | 403 / 401，`m.weibo.cn` 直接回「Sina Visitor System」 | 排除 |
| 人民网 / 新浪 / 新华社 RSS | 200 但 pubDate 停在 2025 或只剩 1 条 | 僵尸源，排除 |
| Google News / HN 官方 RSS / V2EX / RSSHub | 000（TLS 或注册墙 / 超时） | 排除 |

> 热搜**没有分类字段**（头条、百度都只有 标题 + 热度）⇒ 左列必然掺体育/娱乐。这是数据源的形状，
> 不是排版问题；要「只挑某类」得换源，别在热搜上做过滤。用户知情并接受。

### 改了什么

| 位置 | 改动 |
| --- | --- |
| `tools/fetch-news.py`（新） | 三路抓取 + `clean()`（去 HTML/实体/emoji）+ `human_hot()`（13147665→1315万）+ 去重 + 原子 `os.replace`；全挂则 exit 1 **不碰旧缓存**；参数 `OUT.tsv [--hot 14] [--tech 30]` |
| `src/scene_news.c`（新） | 两列各一源；`fit()` 按**真实像素**二分截断标题（CJK 不能用按字符算的截断）；`fmt_age()` 刚刚 / Nm / Nh / Nd；排版计划缓存（键含 mtime/size/列宽/分钟）；顶栏「新闻」+「N 分钟前同步 · 热点 N · 科技 M」，>90 分钟转黄 |
| `src/theme.h` | `L_NW_Y0` / `L_NW_HEAD_H 26` / `L_NW_ROW_H 40` / `L_NW_RANK_W 26` / `L_NW_RIGHT_W 132` / `L_NW_ROW_PAD_MAX 8` |
| `src/scene.h` · `src/scenes.c` · `Makefile` · `src/panel.c` | 注册幕 + `scene_news_file()` + 配置键 `news=` |
| `systemd/hpopi5b-news.{service,timer}`（新） | `OnBootSec=3min` / `OnUnitActiveSec=30min` / `Persistent=true` |
| 真机 | 装了二进制、`fetch-news.py`、两个 unit ⇒ `hpopi5b-news.timer` enabled；`hpopi5b-kiosk` 重启后自动 3 幕轮换（不需要改 kiosk 脚本） |

### 判据数字

- 真屏 `:0`：`probe-frames 50 30` ⇒ **>30% 像素变化的帧 = 0**；`pgrep -cx hpopi5b-panel` = 1 ·
  `/sys/class/tty/tty0/active` = `tty1` · `pgrep -cx slick-greeter` = 0。
- 格式验收：真屏截图里两列都正常（左 01–14 带「1428万」，右 01–15 带「Solidot · 21h」），
  顶栏「16 分钟前同步 · 热点 14 · 科技 30」跟着缓存 mtime 走。
- 几何（`LAYOUT_DEBUG=1`）：`news: cw=617.6 y0=94.0 avail=609.7 rows=15` ⇒ 15×40 = 600，零头 9.7 均摊 0.65px/行。
- 单帧绘制（DEBUG 的「绘制 Xms」）：news 幕 ≈ **6 ms**（比 panel 的 28 ms 便宜，纯文字没曲线）。
- 真屏 CPU（1.8GHz 固定、120s 窗口、3 幕轮换 vs 钉住 panel）：**3% vs 2%**；
  与第十六轮的 A/B 同口径（旧 2 幕 2.9~3.2% / 新 3 幕 3.1%）⇒ **新增一幕在噪声内**。

### 踩了什么

1. `%ldd` 装进 `char age[16]` 触发 `-Wformat-truncation`（编译器怕 17 字节）。正解：天数**封顶 999**，
   而不是加宽缓冲（那只是换个地方溢出）。
2. **`pkill -x hpopi5b-panel` 把真机 :0 实例一起杀了**（测试实例与真机实例同名）。
   kiosk service 的 `Restart=always` 拉回来了，但这条 AGENTS.md 红线自己又踩了一次 —— 以后一律
   `for p in $(pgrep -x ...); do grep -qa 'DISPLAY=:9' /proc/$p/environ && kill $p; done`。
3. `ssh hpopi5b '... 面板 &'` 会让 ssh 会话挂住（两次 600s 超时）。离屏冒烟改成
   `timeout 3 ./hpopi5b-panel ... >/tmp/x.log 2>&1`，别用 `&`。
4. 幕没有 `--news` CLI（只有配置键 `news=`），离屏验证得用真机默认缓存路径 —— 与 `trending` 一致，不算坑，但会绊人。

### 下一轮的候选

- 36氪 feed 解析 0 条（结构不是标准 `<item>`？抓下来看一眼就能修）。
- 国内热点只有 14 条 < 一列能放的 15 行 ⇒ 偶发最后一行空着（下一轮可把 `--hot` 调到 15，或让左列末行留白更自然）。
- 「热点 14 · 科技 30」是**池子大小**，上屏各 14/15 —— 容易让人以为显示错了，可按上一轮经验改成「共 N 条」。

---

## 第十六轮：GitHub 趋势幕填满整屏（2026-11-06 夜）

**用户口径**（m00001）：「GitHub 趋势那一幕我感觉还是有很多空白空间 内容还能加多 你利用一下」

### 改了什么

| 位置 | 改动 |
| --- | --- |
| `src/view.c` / `src/view.h` | 新增 `v_text_measure(fd, maxw, maxlines, s)`：只量不画（折行参数与 `v_text_wrap` 一致） |
| `src/theme.h` | `L_TR_PERCOL` 删除，换 `L_TR_Y0` / `L_TR_NAME_GAP` / `L_TR_RULE_B` / `L_TR_ROW_GAP` / `L_TR_RANK_W` / `L_TR_DESC_LINES` / `L_TR_LANG_H` / `L_TR_LANG_TXT_H` / `L_TR_ROW_PAD_MAX` |
| `src/scene_trending.c` | 固定网格 → **流式两列**；一列宽 635.7 时 8/7 分列；**15 条全上屏**；行尾细线随行高走；标题行「N 分钟前同步 · 共 M 条 · 今日 +X★」；右列尾部**语言分布**脚注 |
| 同上 | **排版计划缓存** `plan`（键 = 列宽 + mtime/size + 条数）：行高、截断名、语言/涨星串及宽度、语言分布行 |
| 同上 | `load()` 去行尾 `\n` 提到切列**之前**（原写法永远去不掉，见 §22） |

### 判据数字

- 真屏 `:0`：`probe-frames 60` ⇒ **>30% 像素变化的帧 = 0**（2.66% 那帧是 30s 换幕）。
- 真屏 CPU（1.8GHz 固定、load 0.1~0.2、`--remote hpcore`、60s 窗口、两幕轮换）：
  旧 3.47% / 3.10%，新 3.97% / 3.12%（**+0.25pp 量级，仍在噪声内**）。
- 单帧绘制（`HPOPI5B_PANEL_DEBUG=1` 的「绘制 Xms」，趋势幕单幕）：旧 19.4ms → 首版 29.0ms
  → **缓存后 23.1ms**。剩下的 3.7ms 是「15 条描述都真排两行」本身的内容量（旧版因 §22 反而少排）。
- 内存 `Pss_Anon` 3.9 MiB；`pgrep -cx hpopi5b-panel`=1；`tty0/active`=tty1；`pgrep -cx slick-greeter`=0。
- 版面：左右两列都收到 `y≈1000`（屏底 1041 之前），右列尾部语言分布条贴到 1040。

### 踩到的两个坑（都是我自己造的）

1. **行尾 `\n` 没去掉**：`load()` 先切 `\t` 再 `strcspn(line, "\r\n")` —— 撞 NUL 就停。
   每条描述背一个 `\n` ⇒ pango 多算一行 ⇒ `H_DESC`=48 而不是 24，屏幕看着正常但排不满。
2. **1 Hz 重画下每帧重新排版**：面板闲时每秒重画（`FEED.age_s` 每秒变），15 次 `v_text_measure`
   + 每行二分截断（≤8 次整形）⇒ 绘制 19.5 → 29.0ms、整机 +1.5% CPU。缓存后代价摊回到「文件更新时一次」。

⚠️ **量 CPU 时踩的第三个隐性坑**：只用 `--remote hpcore` 跑趋势幕时，远端若没连上就**一次都不重画**
（`content_sig()` 不含变化量），于是旧版量出 0 draws / 2.2%、新版量出 30 draws / 3.4%，看起来像回归
却其实是「重画次数不同」。⇒ 判 CPU 要么比「**每帧绘制 ms**」，要么两边都强制同频重画（`SIGUSR2` 每秒一次）。

### 部署

tar → `hpopi5b:/tmp/panelb` → `make panel`（零 warning）→ `install` 二进制 + 探针 → `systemctl restart hpopi5b-kiosk`。

---

## 第十五轮：容器块换成 Tailscale 尾网块（2026-11-06 傍晚）

**用户口径**（m01132）：「不对啊 容器好像不就是进程吗 那算了 不要了 换成别的吧
你看看可以显示 tailscale 的相关东西 看看怎么做比较好」

**为什么「容器就是进程」这个判断在这里是对的**：容器里的进程**本来就出现在 `/proc` 里**（hpcore 上
`magpie` 的 pid 1818752 同时出现在进程榜和容器清单里），所以「容器清单」和进程榜在高负载时信息高度重复；
真正只有 cgroup 能给出的增量是**聚合口径**（一个容器 = 一堆进程之和），而面板已经用进程榜表达了同样的东西。

**尾网比容器清单更值得占这块版面**：hpcore 是机队唯一的入口，7 台设备从这里进出（手机/平板/笔记本/
两台云主机），「谁在线、走直连还是绕 DERP、此刻在传多少」是日常真正要看的；而且这些信息**进程榜完全看不到**。

**怎么做（数据通路）**

- **只读 localapi**：`GET /localapi/v0/status`，走 `/var/run/tailscale/tailscaled.sock`
  （权限 0666 ⇒ 不需要 root）。`IO::Socket::UNIX` + `HTTP/1.0` + `Connection: close`，
  与 docker.sock 那套同样的写法，但**不 fork CLI**（`tailscale status` 要起一个进程、几百毫秒）。
- ⚠️ **JSON 里对端列表的键是 `Peer`（单数）**，不是 `Peers` —— 写成 `Peers` 会静默得到 0 台（我第一次就踩了）。
- 每台：`HostName` / `OS` / `Online` / `CurAddr`（非空 = 直连）/ `Relay`（DERP 区域）/ `LastSeen` / `RxBytes` / `TxBytes`；
  自身 IP 在顶层 `TailscaleIPs[0]`。
- 速率 = 相邻两帧累计字节差 / 帧间隔（探针每 3 秒采一次，按 nodekey 记上一次的值）。
- `LastSeen` 是 RFC3339 带纳秒 + 时区 ⇒ 自己用 `Time::Local::timegm` 减偏移算 epoch（`JSON::PP` 是 5.14 起核心模块）。

**改了什么**

- `src/metrics.h`：删 `ctr_row_t`/`CTRN`/`ctr*`，加 `TSN 12` + `ts_peer_t{name, os, relay, online, direct, last_s, rx_bps, tx_bps}`
  + `ts_ok / ts_total / ts_online / ts_self_ip / ts[] / nts`。
- `src/remote.c`：删 `parse_D`/`parse_ctr_row`，加 `parse_ts_row`（定长 8 段）+ `parse_T`。
- `src/theme.h`：`L_CTR_*` → `L_TS_GAP 18 / L_TS_TITLE_H 24 / L_TS_ROW_H 22 / L_TS_ROWS 5 / L_TS_PAD_B 16`。
- `src/view_panel.c`：`draw_ctr_block` → `draw_ts_block` + `fmt_age`；`ctr_h` → `ts_h`。
  列位从右往左算：速率 100 → 连接方式 62 → 系统 52 → 名字吃剩下的。
- `tools/probe-remote.pl`：删容器段（`docker_names` / `containers`），加 `TS_SOCK / iso_epoch / ts_status`；
  输出 `T <总台数> <在线数> <本机IP> <name>:<os>:<直连0|1>:<DERP区域>:<在线0|1>:<离线秒>:<rxBps>:<txBps>,…`
  （在线优先、再按流量降序、最多 10 台；tailscaled 临时不可用时沿用上一帧，不闪空）。

**判据（真屏 hpopi5b，`--scale 1.48`）**

| 项 | 结果 |
| --- | --- |
| 编译 | hpopi5b `make panel` **0 warning / 0 error** |
| 版面预算 | `LAYOUT_DEBUG=1` ⇒ `wa=399.2 ch=135.4 FIX_A=187 FIX_B=187 余=0.0` |
| 尾网块 | 9 台设备（7 在线 / 2 离线）、`直连 4 · 中继 2`、离线行「3h 前 / 16h 前」、速率按帧变化 |
| 探针 | `perl -c` OK；本机实测 `T 9 7 100.64.0.1 hpbook:macOS:1:hpnet:1:0:469:403,…`（名字/系统/区域解析正确） |
| 不闪 | `DISPLAY=:0 tools/probe-frames 60` ⇒ `>30% 变化帧 = 0` |
| 服务 | `is-active=active`、`pgrep -cx` = 1、`vt=tty1`、`greeter=0` |
| 部署件一致 | `sha256sum` 构建产物 vs `/usr/local/lib/...`、探针 vs `/usr/local/share/...` 各 2 行同哈希 |
| CPU | 90s 窗口 `utime+stime` ⇒ **3.43%**（与第十四轮 3.3~3.7% 同档；`VmRSS/VmHWM 20.4 MB`） |

**踩的坑（本轮新增）**

1. **JSON 键是 `Peer` 不是 `Peers`**：写错不报错、只是「0 台设备」，白查一轮。
2. **「系统」列左对齐会叠字**：`windows` 比 38px 槽位宽，直接压到右对齐的「中继 hpnet」上；
   改成右对齐 + 槽位 52（`pngcrop` 放大后才看清是叠字，整图缩着看不出来）。
3. 写注释时把 socket 权限误写成占位符，已改成明文 `0666`。

**留给下一轮**

- 尾网块只画 10 台（`L_TS_ROWS 5 × 2`）；超过 10 台时标题行的「N 台」会大于画出来的行数（现在 9 台，不冲突）。
- 没有出口节点（`ExitNodeStatus`）/ DERP 延迟（要 `netcheck`，那个很贵）；没有「本机自身流量」。
- `Relay` 为空时画「中继」不带区域名（tailscaled 没给区域时的兜底）。
- 进程榜仍是最贵的绘制项（25 行 × 2 次 pango 整形）；再降 CPU 的正解还是 PangoLayout 缓存 + 区域局部重绘。

---

## 第十四轮：一条进程榜 + 容器块 + 曲线降高（2026-11-06 下午）

**用户口径**（m00842）：「我没搞懂 你为什么要搞两个一模一样的进程 列表 你直接整列显示 二十多个不就好了吗
另外两列空余空间也很多 一个折线图占位太多了 继续想办法 先把那个进程列表搞定吧 也不用占那么大空间吧
你可以把我们的 Docker 容器都列出来呀 搞一块地方 显示资源占用之类的」

**四条都对**：上段（按内存）与下段（按 CPU）画的是同一批进程、字段一样，只有顺序不同 ——
一行信息花了两行版面；而 hpcore 是 all-in-one，**容器才是这机队唯一真在跑的东西**，
面板里却只有一个「容器 23」的数字。

**改了什么**

- **一条榜**（`src/view_panel.c::draw_proc_list`，原 `draw_proc_section`）：只调一次，
  数据用 `m->top_cpu`，行数按可用高度算 `(bottom - BODY_Y - TITLE_H - 10) / ROW_H`
  ⇒ 1080p 下 **25 行**（`L_PROC_MAX 26` 封顶）。每行「名次 · 名称 pid · 内存 · CPU%」。
  CPU%：`<0`（老探针没这列）画 `-`、`<100` 一位小数、`≥100` 取整、`>80` peach。
- **容器块**（`draw_ctr_block`）：横跨主机区整宽、贴在主机块下沿，两个子列各 5 个（共 10），
  标题「容器 N 个 合计 xG … 内存 / CPU（近 3 秒）」。没有容器时 `ctr_h = 0` ⇒ 整块不画、曲线自己长回去。
- **曲线降高**：`L_H_CHART_MAX 260 → 150`；`L_HOST_FRAC 0.56 → 0.66`；新增
  `L_PROC_TITLE_H 28 / L_PROC_ROW_H 24 / L_PROC_MAX 26 / L_CTR_GAP 18 / L_CTR_TITLE_H 24 /
  L_CTR_ROW_H 22 / L_CTR_ROWS 5 / L_CTR_PAD_B 16`；删 `L_PROC_ROWS / L_PROC_COLS /
  L_PROC_SEC_H / L_PROC_SEC_GAP`。
- **探针**（`tools/probe-remote.pl`）：`Q` 行 24 条 ≥2% → **40 条无阈值**（空载时自动退化成内存榜）；
  新增 `D <总数> <合计MiB> <name>:<memMiB>:<cpu%>,…`：glob `/sys/fs/cgroup/system.slice/docker-*.scope`，
  读 `memory.current` 与 `cpu.stat` 的 `usage_usec`（差值/hz/秒 ⇒ 1.0 = 一个核），
  名字从 `docker.sock`（`GET /containers/json?all=1`，`Connection: close` 避 chunked，缓存 300s）翻，
  翻不到显示 ID 前 12 位。`perl -c` 通过；`sudo -n perl tools/probe-remote.pl` 实测
  `D 23 3520.0 magpie:977.7:325.7,cormorant-browser-1:868.8:0.1,…`（23 个容器名全部解析成功）。
- **解析**（`src/remote.c`）：`parse_D()` + `parse_ctr_row()`（3 段：name / mem / cpu）。
  `metrics.h` 新增 `ctr_row_t` / `CTRN 12` / `nctr / nctr_total / ctr_mem_total_mb`。

**判据（真屏 hpopi5b，`--scale 1.48`）**

| 项 | 结果 |
| --- | --- |
| 编译 | hpopi5b `make panel` **0 warning / 0 error** |
| 版面预算 | `LAYOUT_DEBUG=1` ⇒ `wa=399.2 ch=138.4 FIX_A=187 FIX_B=187 余=0.0` |
| 进程榜行数 | 25 行（末行最低墨迹 699 逻辑像素，`bottom = 712`） |
| 容器块 | 10 个、名字完整（`cormorant-browser-1` 不截断）、最低墨迹 703 逻辑像素（没被屏边切） |
| 不闪 | `DISPLAY=:0 tools/probe-frames 60` ⇒ `>30% 变化帧 = 0` |
| 服务 | `is-active=active`、`pgrep -cx` = 1（测试实例已按 `DISPLAY=:9` 挑出 kill）、`vt=tty1`、`greeter=0` |
| 部署件一致 | `sha256sum` 比对构建产物与 `/usr/local/lib/...` ⇒ 二进制、探针各 1 行（`uniq`） |
| 老格式兼容 | `parse_proc_row` 按冒号段数分流：3 段 ⇒ `cpu_pct = -1` ⇒ 画 `-` |

**踩的坑（本轮新增）**

1. **看缩放过的截图判版面**（第三次了）：我把「曲线在框内」误读成「掉到框外」、把「刻度在顶部」误读成
   「在底部」、把「剩 9px」误读成「被切掉」。修法写进 `.ai/pitfalls/cases/2026-11-06-eyeballing-scaled-screenshots.md`：
   **先算 `LAYOUT_DEBUG`，再 `pngcrop` 数像素，最后才看图**。
2. **`docker Root Dir = /data/docker`**（不是 `/var/lib/docker`）⇒ 想靠猜 `config.v2.json` 路径拿容器名
   会扑空；happpy **不在 docker 组**（`docker ps` permission denied）但 **`sudo -n` 可用**。
   正解是探针用 `IO::Socket::UNIX` 直连 `/var/run/docker.sock`（探针以 root 跑）。
3. **cgroup 目录名是 64 位容器 ID**，和 `docker ps` 的短 ID 对不上 ⇒ 必须靠 socket 的 `Id` 全串匹配。
4. `ssh -T hpopi5b '<cmd> &'` 不加 `-T` 与 `< /dev/null` 时 ssh 会挂着等 pty ⇒ 工具调用 60s 超时；
   后台任务用 `setsid ... < /dev/null > log 2>&1 &`。

**留给下一轮**

- 容器只按内存取前 12 个（面板画 10 个）；要「按 CPU 排容器」得再加一行契约。
- 容器健康度（restart 次数 / OOM / 退出码）没做 —— 需要 `docker inspect` 或 cgroup 的 `memory.events`。
- 进程榜仍是最贵的绘制项（25 行 × 2 次 pango 整形）；再降 CPU 的正解还是 PangoLayout 缓存。
- `%s %s` 的「0M」：rss < 1MB 的进程显示 `0M`（`v_fmt_mb` 取整），要精确就到 `0.4M`。

---
## 第十三轮：单机化（all-in-one hpcore）+ 把屏幕占满 + 进程榜整列给它（2026-11-06）

**用户报障/口径**（m00001）：「hpcmd 下线了 现在所有都是 all in one hpcore 了 所以 那一幕你看看显示点啥
比较好 全显示 hpcore 相关东西 把空间占满 进程那一列可以全部给他了」

**为什么必须改**（不是审美问题）

- `hpcmd`（192.168.5.51）实测 `ping` 100% 丢包；而 `/etc/hpopi5b-panel.conf` 还是
  `remote=hpcore` + `remote2=hpcmd` ⇒ 左列永远是 `● 离线 55983s`，日志每秒一行 `远程 hpcmd: ssh 已退出`。
- 三列版式下曲线只有 **96** 逻辑像素高，而这台机 CPU 常年在 2~5% ⇒ 曲线上方 ~90 像素恒白。
  「空间占满」说的就是这个：**固定量程 + 低负载 = 一大片会被读成「没画东西」的空白**。

**改了什么**

- 版面（`src/view_panel.c` 重写）：主机区两子列，左 = 身份/CPU（行+条+每核柱+曲线）/内存（行+条+曲线），
  右 = 网络（行+双曲线）/磁盘 I/O（行+双曲线）/磁盘用量（行+条）/容器·线程·进程/累计/PSI。
  两子列**固定高度必须相等**（`FIX_A = FIX_B = 187`，`LAYOUT_DEBUG=1` 会打出来），否则曲线基线错开。
- 曲线向上吃满：`ch = (avail - 187 - 2*6) / 2` ⇒ 1080p 下 **219.4**（clamp [30, 260]）。
- 低负载空白问题：新增 `v_chart_scale()`（`src/view.c`）画 25/50/75% 三条极淡细线 + 右上角量程，
  并把量程**显式**回传给 `v_spark/v_spark2`（网格与曲线不再各算一套）。
- 进程榜（`src/metrics.[ch]` + `src/remote.c` + `tools/probe-remote.pl`）：
  `proc_row_t` 加 `cpu_pct`；本机按 `/proc/<pid>/stat` 的 utime+stime 差值算，远端探针 `P` 行扩成
  `pid:rss:cpu%:name` 4 段、新增 `Q` 行（按 CPU 的榜）。面板按冒号段数兼容 3 段老格式
  （老探针 ⇒ CPU 列画 `-`，**不冒充 0.0%**）。
- 单机化：`CFG_REMOTE2`/`R2`/`FEED2`/`env->r2`/`env->feed2`/poll 第三源全部删除；
  `--remote2` 与 `remote2=` **仍然接受**但只打一行「已废弃，已忽略」（老 conf 在 hpopi5b 上就有）。
- 省一笔：屏幕上画的是远端那台时，本地进程榜一个像素都用不到 ⇒ 不再每 3 秒扫一次 `/proc`
  （实测省下的 CPU 在噪声里，但「不采集用不到的东西」本身是对的）。

**判据（真屏 `:0`，2026-11-06 16:2x）**

| 判据 | 结果 |
| --- | --- |
| 不闪 | `tools/probe-frames 60` ⇒ **>30% 变化帧 0** |
| 主循环 | `2 it/s`、poll 全超时、事件 0、远端帧 1/s、重绘 1/s |
| 几何 | `LAYOUT_DEBUG=1` ⇒ `wa=336.2 ch=219.4 FIX_A=187 FIX_B=187 余=0.0` |
| CPU（A/B，1.8 GHz 固定频率） | 旧三列 **3.47%** → 新单机 **3.68%**（+0.2pp；同窗口先后量） |
| 内存 | `VmRSS 20.6 MB`（`VmHWM` 同值，无增长） |
| 红线 | `is-active`=active、`pgrep -cx`=1、`tty0/active`=**tty1**、slick-greeter=0、无 X 错误 |
| 探针 | `P` 4 段 + `Q` 行实测可用（`yes` 压一核时 `Q` 里 `75.5%`），空载时下段是真实的小数值 |

**⚠️ 本轮挖出来的两件事（都不是本次改动引入的）**

1. **`1% of one core` 这个验收数字在 hpopi5b 上量不出来**：Pi 的 cpufreq 是 `ondemand`，
   空载跑 **408 MHz**，同一份二进制按 CPU-秒算贵 ~5 倍（同一份代码：408 MHz ⇒ 5.8%，
   1.8 GHz ⇒ 3.7%）。⇒ 以后只能 **A/B 比**（`docs/verify.md` 五节已写清怎么固定频率）。
   旧二进制的 3.47% 说明「1%」本来就不可复现（文档里那个 0.60% 是 hphelm 上的数）。
2. **`/tmp/render_tr` 有 5 个孤儿进程各烧 100% CPU、已烧 3 天**（11-05 14:02 起的测试残留，
   `load average` 5.2）。已 `kill -9`。它们把 Pi 5 个核吃满、把 load 顶到 5，会让任何 CPU 测量失真。
   ⇒ 量性能前先 `ps -eo pcpu,pid,comm --sort=-pcpu | head`。

**踩的坑**

- `pkill -f "Xvfb :9"` 在 `ssh hpopi5b '…'` 里会把**执行它的远端 shell 自己**匹配上 ⇒ ssh 直接 255、
  什么都没跑（AGENTS.md 第 7 条，第五次）。修法：写脚本文件执行 + `pkill -x`。
- `ssh hpopi5b '<cmd> &'` 不加 `-T` / stdin 重定向到 `/dev/null` 时，ssh 会一直挂着等 pty，
  我的工具调用因此 60s 超时。
- 量 CPU 时「旧版」那次 `cp` 撞上 `Text file busy`（服务还在跑）⇒ 那一轮量到的其实还是新版。
  换二进制**必须先 `systemctl stop`**。
- `tools/render-ascii.py` / `measure.py` 依赖 ImageMagick，hpopi5b 上**没有 magick** ⇒ 这条路的
  「量像素」暂时用不了；本轮改用 `tools/xshot.c`（新写的纯 X11 截图工具，零新依赖）+ 目测 + 代码算。

---

## 第十二轮：第 4 格曲线从「温度」换成「磁盘 I/O」（2026-11-05）

**用户报障**（m00001）：「资源面板 hpcmd 磁盘上方的折线图不显示，hpcore 有。是因为 hpcmd 是 hpcore 的虚拟系统吗」

**诊断**（两条都对，且比他猜的更糟）

- 那条曲线是**温度**。两处读取端都只认 `hwmon name == k10temp`：`src/metrics.c` 的 `read_temp()`、
  `tools/probe-remote.pl` 的 `temp_c()`。
- `hpcmd` 确实是 `hpcore` 上的 KVM guest（`virsh list --all` ⇒ `hpcmd running`；`systemd-detect-virt` = kvm；
  DMI = QEMU）。它 `/sys/class/hwmon` **整个是空的** ⇒ 永远没有温度。
- 部署机 Pi（hpopi5b）的 hwmon 叫 `soc_thermal`/`*_thermal`，也不叫 k10temp ⇒ **三列里只有 hpcore 画得出来**。
- 顺带查出两个更大的洞：① `read_disk()` 只认 `/sys/block/nvme0n*`（Pi 是 `mmcblk1`、hpcmd 是 `vda`）
  ⇒ 那两列连「SSD 累计读/写」都是空的；② `remote.c` 对探针的**追加段从未解析** ⇒ 远端列的
  缓存 / PSI / 线程数 / 累计流量 / 丢错包 **永远是 0**（拿假 0 冒充真数据）。

**改了什么**（用户裁决「可以」，m00071）

- **第 4 格 = 磁盘 I/O 双曲线**：teal 读 / peach 写（与网络 ↓/↑ 同色系），共享量程。
  该格不设标题行：颜色即图例，当前值写在下面磁盘行（同色「读 X 写 Y」）。理由写进了
  `src/view_panel.c` 的注释（温度已有 °C 文本 / 无传感器机器整格空白 / 磁盘速率是唯一缺的维度）。
- `read_disk(m, dt)`：扫 `/sys/block` **所有整盘** stat（扇区 × 512，求和），跳过
  `loop/ram/zram/nbd/sr/fd/dm-/mtdblock`；速率走单调时钟 `dt`（与 `read_net` 同口径）。
  字段改名 `have_ssd`→`have_disk`、`ssd_{read,written}_gb`→`disk_*`（Pi 上那块是 SD/eMMC，叫 SSD 是错的）。
- 磁盘行右侧改成速率；**盘累计 GB 挪到脚注**并改称「盘 读 X 写 Y」（慢变值不该占当前值的位置）；
  `剩 X / Y` 里的大数也统一用 T（原来 free 恒用 G ⇒ 「剩 2197G / 3.6T」）。
- `tools/probe-remote.pl`：新增 `disk_rw()` + 两个尾部字段 `diskReadBps diskWriteBps`（仍遵守「只追加」）；
  `src/remote.c`：`S_MAX_FIELDS` 32→36，**补全整个追加段的解析**（t[22..32]，老探针缺字段仍是 0）。

**判据（hpcmd 上能做的都做了）**

- 编译：`gcc -O2 -Wall -Wextra` 编 `view_panel.c/view.c/theme.c/metrics.c` **零告警**，`remote.c` `-fsyntax-only` 通过。
- 版式（离屏 harness 直调 `view_panel_draw` → PPM → 自写 stdlib 分析器，因为 hpcmd 没有
  Xvfb/make/pkg-config/ImageMagick/PIL）：
  - 每列 **4 张曲线**（CPU 202 / 内存 338 / 网络 456 / I/O 549 逻辑 y），`ch = 87.4`、`余 = 0.0`；
  - I/O 格两个颜色都在：远端 teal 845–888、peach 928–934（写 12M vs 读 84M ⇒ 共享量程下贴基线，是设计本意）；
    本机 `io_wr = 0` 时 peach 只有贴底 1 行（诚实零基线，不是画丢了）；
  - 白天主题（`theme_set(0)`）同样两色可见（1948 / 1036 px）；
  - 文字无重叠：磁盘行整行墨迹 0.7→285 逻辑 px，脚注到右对齐「压力」还有 **93 逻辑 px** 空隙。
- 探针：`perl -c` OK；在 hpcmd 实跑输出 **33 字段**（尾部出现 `.8M/s` 写速率），`tempC = -1.0` 也印证了没有 hwmon。

**真机判据（hpopi5b，按 `docs/deploy.md` 三十秒版，2026-11-05）**

- 构建：`make panel`（aarch64，本地编）**0 warning**；备份 `hpopi5b-panel.prev` 102272 B +
  `probe-remote.pl.prev` 10132 B；`systemctl restart hpopi5b-kiosk` ⇒ `is-active` active、
  `pgrep -cx hpopi5b-panel` = **1**、日志「已连上（hpcmd 4 核 / hpcore 16 核）」、
  `grep -cE "X Error|Segmentation"` = **0**。
- **屏幕取证**（Pi 上 `XGetImage` → PPM → stdlib 分析）：第 4 格已是**满宽双曲线**（peach 写曲线
  横跨 x 26..659，teal 读曲线有尖峰），磁盘行 =「`磁盘 6% 剩 …` + teal `读 0B` + peach `写 988B`」，
  脚注 =「盘 读 …G 写 …G」；两列都对位（磁盘行 y 967-991、条 997-1009、脚注 1027-1049，无重叠）。
- **「读≈0」是真实的**，不是解析 bug：hpcore/Pi 的 `/sys/block/*/stat` 5 秒内**读扇区一动不动**、
  只有写扇区在涨（读全走 page cache）。
- **CPU（Xvfb :9 同条件 A/B，各 25s 窗口，交替两轮）**：旧 5.12/5.08% → 新 5.52/5.40%
  ⇒ **+0.35 pp（约 3.5 ms/s，+7% 相对）**；RSS 17940 → 17908~18008 kB（噪声内）。
  代价来自「每秒多读 5 个 `/sys/block` 小文件 + 每帧多一条 sparkline 描边」，与估计一致。
- ⚠️ 改 `metrics.h` 结构 ⇒ 真机必须**整体重建**（`make panel`），不能只换二进制跟旧目标文件混链。

**判据坑（已修文档）**：kiosk 每 30s 换幕，那 2 帧必然 100% 像素变化 —— 直接套
「`probe-frames` >30% 帧必须 0」会把换幕判成闪（同一条命令连跑三次：2 / 0 / 3）。
已在 `docs/verify.md` §二、`docs/deploy.md`（三十秒版 + 九条判据表）、`AGENTS.md` 括注
「换幕帧除外 / 用 `--rotate 0` 的隔离实例量」，记忆库
`.ai/pitfalls/cases/2026-11-05-probe-frames-vs-scene-rotation.md`。

**踩的坑**：离屏 harness 首次出图**全黑** —— 忘了 `theme_set()`，`TP` 调色板全 0（黑底画黑字）。
记进 `.ai/pitfalls/cases/2026-11-05-offscreen-harness-all-black.md`（含「红矩形对照」判据与完整编译命令）。
另：`pkill -f "Xvfb :9"` 又一次把执行它的 shell 自己杀掉（第 6 次）—— 用 `pgrep -x` + 按
`/proc/<pid>/cmdline` 挑 pid。

**文档**：`README`（面板能力表 + 数据源）、`docs/design.md`（版面 ASCII、四张曲线清单、色语义）、
`docs/architecture.md`（温度只有文本 / 磁盘扫所有整盘）、`.ai/decisions/2026-11-05-chart-slot-disk-io.md`。

---

## 第十轮：改名 `hpopi5b-panel` + 归位 `/data/dev/project/`（2026-11-05）

**用户口径**（m00001）：项目不该平铺在 `/data/dev/` 顶层，应在 `/data/dev/project/`；`hphelm-panel`
这个名字已随 `hphelm` 退役而失真，改名 **`hpopi5b-panel`**，副作用（代码/仓库/生产/kb/项目记忆）一并处理。

**做了什么**

- 目录：`/data/dev/hphelm-panel` → **`/data/dev/project/hpopi5b-panel`**（同批把 `chime`、`herdr-lark`、
  `cormorant` 归位到 `project/`；`cormorant` 等它的会话结束才动）。
- GitHub：`zeroicey/hphelm-panel` → **`zeroicey/hpopi5b-panel`**（`gh repo rename`，旧地址自动 301）。
- 代码内标识统一：二进制 `hpopi5b-panel`、认证助手 `hpopi5b-auth`（`src/hpopi5b-auth.c`）、
  `HPOPI5B_{PANEL_DEBUG,REMOTE,PROBE,AUTH_BIN}`、安装路径 `/usr/local/{lib,share}/hpopi5b-panel/`、
  `/etc/hpopi5b-panel.conf`、`/var/{log,cache,lock}/hpopi5b-panel*`、单元 `hpopi5b-kiosk.service` +
  `hpopi5b-trending.{service,timer}`。
- **纯历史称谓保持原样**：旧工位机 `hphelm`、已废弃的 `hphelm-session` / `hphelm-vt-guard` / `hphelm-lockd`
  不改写（它们描述的是 2026-10-27 之前真实存在过的东西）。
- 生产（hpopi5b）同批迁移：旧路径留 `.bak-20261105`；二进制由真机 `make panel` 重建后安装。
- 文档：`README` / `AGENTS.md` / `docs/deploy.md` 的「三十秒版」已切到 **hpcmd 改 → push → hpopi5b 编译安装**；
  旧 hphelm 原地部署段落标注为历史。

**判据**：`hpopi5b-kiosk` active、`pgrep -cx hpopi5b-panel`=1、`DP-1` 带 `*`、`probe-frames` 0 闪帧、
trending timer 下一次触发正常、日志无 X Error。

---

## 第九轮：多幕（scene）架构 + 轮换（2026-11-05）

**用户口径**（m00046）：「不要一个写死的面板」——把「只有一块屏/一种版面」抽象成**可插拔的多幕**，
先随便显示几幕 + 特效轮换看效果，并问轮换时间怎么定。

**做了什么**

- 新接口 `src/scene.h`：`scene_t{id,label,draw,sig}`；`draw` 拿到**已 scale** 的 cairo 上下文、
  底图已铺好（幕不碰 X、不铺底）；`sig` 是幕自己的内容签名。
- 新调度 `src/scenes.c`：单一参数 `--rotate S`（kiosk 默认 30 s，桌面/锁屏默认 0=不轮换），
  转场 0.55 s，特效循环 `cut→dip→push→cover`；`scenes_resume()` 在回到面板时打断转场重开计时。
- 四幕：`panel`（原三列版面，转调 `view_panel_draw`）·`hero`（两行主机 × 四个 80pt 数字）·
  `clock`（200pt HH:MM + 日期 + 分钟进度线）·`ambient`（近黑，一分钟才重画一次）。
- `panel.c`：`render()` 改成「幕 → back」；转场时才懒分配 `altpix`（**+8 MiB 仅在人真的看轮换时**）；
  转场期间 `poll` 超时 500 ms → 30 ms；底部统一小点 HUD（只有轮换开着才画）。
- 参数/配置：`--scene --rotate --transition --list-scenes`；`/etc/hpopi5b-panel.conf` 支持
  `rotate= scene= transition=`（先读文件、再让命令行覆盖）。
- `Makefile` 加 5 个 obj，模式规则加 `scene.h` 依赖。

**判据（2026-11-05，真机 hpopi5b 已上屏）**

- 构建：真机 `make panel` 零告警（aarch64 / gcc 11）。
- 真机 `:0`：`probe-frames 60` ⇒ **>30% 变化帧 = 0**；`pgrep -cx hpopi5b-panel = 1`、`tty0/active = tty1`、
  日志无 X Error / BadMatch / Segmentation。
- 轮换确实在跑：间隔 33 s 抓两张帧，`diffpx = 351,706 (17.0%)`（同一幕内不会这样）。
- 性能（:9 上同条件对比新旧二进制，各 15 s）：**新 4.40% / 旧 4.46%**、RSS 18.2 MiB、线程 1。
  （`:0` 上量到过 6.7%，那是同时跑 `probe-frames`（每秒 ~15 次 XGetImage 全屏）造成的，不要拿那个数当基线。）
- 转场：四种特效各跑 7 s 无 X 错误；push 的中间帧确认「A 在左、B 从右露出」。
- 四幕真机截图逐张看过；`hero` 里 hpopi5b 的 TEMP 显示「—」是**诚实的空态**（Pi 没有可读温度传感器）。
- 部署落点（真机事实）：源 `/root/hpopi5b-panel`、二进制 `/usr/local/lib/hpopi5b-panel/hpopi5b-panel`
  （`hpopi5b-panel.prev` 是 11-03 的 82,176 B 旧版）+ `hpopi5b-kiosk.service`（xinit 起 Xorg :0 vt1）。
  ⚠️ 仓库里的 `tools/deploy.sh` 仍指向 hphelm 的 `/usr/local/bin`，**在 hpopi5b 上不能用**。

**追加（同日）：`trending` 幕（GitHub 趋势）**

- 已确认：GitHub **没有 trending API**（`/search/repositories?q=created:>…` 是「新建仓库涨星」，
  看不见老仓库翻红）。所以抓 `github.com/trending?since=daily` 的 HTML（Pi 直连 200，16 条）。
- `tools/fetch-trending.py`：urllib + regex → TSV，**原子替换**（`os.replace`），去 HTML 标签/实体/emoji；
  抓不到返回非 0 ⇒ systemd 记失败，缓存保留上一份。
- `scene_trending.c`：只读缓存，`stat` 比对 mtime/size 才重读；sig 混入「分钟」（相对时间要更新）；
  按真实像素宽截断（`v_text_h` 同款思路：CJK 不能按字符数算宽）；总数 `313,652 → 313.7k`。
  空态写「还没同步 + 该跑哪个 service」，**不放假数据**。
- `systemd/hpopi5b-trending.timer`：`OnBootSec=2min` + `OnUnitActiveSec=3h`（日榜一天只抓一次太旧）。
- 真机：timer 已 enable，首次抓取 16 条；转场轮到第 3 幕时实拍确认列表/涨星/总数/HUD 全部正确。
- 踩到：`atol("313,652")` 在逗号处停下 ⇒ 总数全变成「313★」。**千分位字符串不要直接 atol**。

**追加（同日）：只留两幕 + trending 两列**

- 用户裁决：hero/clock/ambient 「撤掉」，只保留 panel + trending（文件已删，git 历史仍可找回）。
  幕表从 5 项降到 2 项，`--list-scenes` / README / AGENTS / architecture 同步。
- trending 改版：**两列 × 6 行 = 12 条**（列宽 `(mw-L_COL_GAP)/2`），仓库名 14pt、描述 **13pt**（新增
  `F_DESC`/`v_fd_desc()`）、语言与「N/日」右对齐在第一行、总星小字落在描述行右端。
- 修 `fit()` 在多字节字符处**死循环**（`mid` 退到 `lo` 后不再前进）：加 `mid == lo` 退出。真机 100% CPU 才暴露。
- 真机判据：0 闪帧、服务单实例、日志无 X 错误；实拍 trending 列表 12 条两列 + HUD 两点（第 2 点高亮）。
- ⚠️ **跨机 tar 同步会让 make 误判「已最新」**：tar 保留 hpcmd 的 mtime（UTC），Pi 是 CST，看起来像 6 小时前。
  同步源码后一律 `make -B panel`（或用 `install -m` / `touch`）。

**追加（同日）：白天/夜间主题**

- `theme.h` 从「一堆 #define 色值」改成**语义宏 → 运行时调色板 `TP`**（`C_MAUVE` 仍是 `TP.mauve`），
  所有版面文件一行没改就跟着换色；色值集中到 `src/theme.c`：夜间 Mocha（原样）、白天 Latte。
- 固定时刻：`theme_night_at(now, 07:00, 19:00)`，每 10 s 看一次钟；跨零点区间也支持。
- 切换：淡出 0.45 s → 全黑时 `theme_apply()`（换 TP + **重画 bgpix**）→ 淡入 0.45 s；
  遮罩画在后备 pixmap 上，上屏仍是单次 `XCopyArea`；淡变期间 poll 25 ms。
- 坑：`bgpix` 里是烤好的渐变，只换宏不重画 ⇒ 必须 `bg_init()`（theme_apply 里做了）。
- 验证：panel/trending × day/night 四张离屏实拍；自动切换用 `--theme-day/--theme-night` 设到下一分钟，
  40 ms 采一次平均亮度：白天 237 → 谷底 5.7 → 夜间 21.5，日志 `主题：夜间` 正好在边界；
  反向（夜间→白天）同样通过。真机重启后 `主题：白天`，`probe-frames` 0 帧 >30%。

**追加（同日）：趋势幕改版 + 只留淡变**

- `trending`：两列 × **5 行**（10 条），描述 13pt **最多两行**（新增 `v_text_wrap()`，Pango
  按宽度折行 + 末行省略，返回实际高度）；总星挪到标题行（`TypeScript · 113.0k★`），
  描述因此可用整列宽。6 行放两行描述会压到分隔线，所以是 5 行/列。
- 转场只留 `dip`（淡出到 `crust` 再淡入）+ `cut`（验版式）。`push`/`cover` 连同 `altpix`
  第二缓冲一起删掉 —— 每次切幕少一次全屏绘制，也不再占 8 MiB。
- 验证：trending day/night 两张实拍（无重叠、两行描述不压线）；`--rotate 4` 抓 60 帧，
  中间帧标准差从 ~40 掉到 **7.5**（整屏被底色糊住）且无水平位移 ⇒ 确实是淡变。
- 坑：`pango_layout_set_height(-2)` 之后必须把 width/height/wrap/ellipsize **复位**，
  否则后续 `v_text()` 全被两行宽度约束带着走。

**下一轮**：让 `tools/deploy.sh` 认出 hpopi5b（路径/服务名 + 安装 `fetch-trending.py` 与 timer），并把 `--rotate` 写进 `/etc/hpopi5b-panel.conf`
（当前靠 kiosk 默认 30 s）；按肉眼感受决定 25/30/45。

---

## 零、先读 `AGENTS.md`

如果你（人或 AI）是来**接手维护**这个项目的：**先读 [`AGENTS.md`](AGENTS.md)** —— 那里有
三条红线、硬约束（性能预算/不许引工具包）、工作流、每轮必跑的验收判据、以及「不要做什么」。
本文（HANDOFF）只记「逐轮做了什么 + 当前待办」。

---

## 一、30 秒上手

**源码目录 = 部署目录 = `hphelm:/data/dev/hpopi5b-panel`**（2026-10-19 整仓迁入；旧的
`hpcore:/data/dev/hpopi5b-panel` 与 `hphelm:~/projects/panel2` 都已不存在）：

```bash
cd /data/dev/hpopi5b-panel
make                     # 需 pkg-config: x11 x11-xcb xscrnsaver cairo cairo-xlib pangocairo
tools/deploy.sh --check  # 看部署会做什么（只读）
tools/deploy.sh          # 构建 → 备份旧二进制 → 安装 → 重启面板 → 跑判据（本机原地）
```

⚠️ **必须在本机跑**：没有 rsync、不 ssh 自己连自己（本机 `~/.ssh/config` 没有 `hphelm` 这个 Host，
`ssh hphelm` 直接 `Host key verification failed`）。屏幕更新怎么带走、怎么回滚、六件绝不能做的事，
全在 **`docs/deploy.md`**。

**先在隔离环境看效果**（不要一上来动真屏）：

```bash
Xvfb :9 -screen 0 1920x1080x24 -ac &
DISPLAY=:9 ./hpopi5b-panel --state panel --lock /tmp/xv9.lock   # ⚠️ --lock 必须换，否则和真机实例抢锁
DISPLAY=:9 import -window $(DISPLAY=:9 xdotool search --name hpopi5b-panel | tail -1) /tmp/shot.png
python3 tools/render-ascii.py /tmp/shot.png 22 22              # 看版式
```

**看效果需要重启进程**（`kill -USR2` 只切状态、**不会重载二进制**）：

```bash
pkill -x hpopi5b-panel        # 会话包装器的守护循环会在 ~1s 内用新二进制把它拉回来
# ⚠️ 别用 `pkill -f`：它会匹配到执行它的那个 ssh shell 自己（已踩四次，见 docs/pitfalls.md）
# ⚠️ 重启会让桌面露出 1~2 秒（面板 unmapped）；`rc=143` 不计失败，红线 2 安全
# ⚠️ 在 Xvfb 里测时不要 `pkill -x`：会连真机实例一起杀（同名），请按 DISPLAY 挑 pid
```

**先用隔离环境试**（不动真屏，推荐每次都先这么干）：

```bash
Xvfb :9 -screen 0 1920x1080x24 -ac &
DISPLAY=:9 ./hpopi5b-panel --state panel     # 或 --state login
DISPLAY=:9 import -window "$(DISPLAY=:9 xdotool search --name hpopi5b-panel | tail -1)" /tmp/p.png
```

**不需要 X 的自检**：`./hpopi5b-panel --dump`（打印采集层数据）。

**验证渲染（我看不到图，所以走像素判据）**：

```bash
# 粗看布局（12x14 px 一格，亮度字符 + C=彩色）
python3 /tmp/ascii2.py /tmp/p.png | head -50      # 注意 /tmp 是易失的，见「四」
# 细看区域（x0 y0 x1 y1 每格像素）
python3 /tmp/fine.py /tmp/p.png 700 560 1230 780 4
# 取某个颜色的 bbox（例如输入框底色 #313244）
magick /tmp/p.png -crop ... ; # 见 README 的方法
```

---

## 二、代码地图（1634 行 C，无 GUI 工具包）

| 文件 | 行数级 | 作用 | 改 UI 常动它吗 |
| --- | --- | --- | --- |
| `src/theme.h` | 50 | **Catppuccin Mocha 配色 + 布局常量（逻辑像素）** | ⭐ 最常动 |
| `src/view.c` | 250 | 绘制基元：文字（pango）/背景渐变/细线/进度条/每核柱/sparkline | 偶尔 |
| `src/view_panel.c` | 190 | **面板版面**（无卡片、三栏 + 进程榜 40 条） | ⭐⭐ 最常动 |
| `src/view_login.c` | 100 | **登录页版面**（无卡片、居中） | ⭐ 改登录页时 |
| `src/metrics.[ch]` | 480 | 数据采集（只读 `/proc`、`/sys`，不 fork） | 加新指标时 |
| `src/panel.c` | 420 | X11 窗口、事件、**状态机**、主循环 | 改交互时 |
| `src/auth.[ch]` | 90 | 与助手通信（密码只走 stdin） | 一般不动 |
| `src/hpopi5b-auth.c` | 120 | setuid PAM 助手 | 一般不动 |

**坐标体系**：所有绘制都在「逻辑像素」下进行，主循环里 `cairo_scale(SCALE, SCALE)`，
`SCALE=1.48`（屏 141.8 DPI ÷ 96）。所以 `theme.h` 里的 `L_MARGIN=18` 实际是 18×1.48≈27 物理像素。
**改布局优先改 `theme.h` 的常量，不要硬编码在 `view_draw.c` 里。**

---

## 三、本轮已定的关键决策（别推翻，除非有实测理由）

| 决策 | 为什么 | 证据 |
| --- | --- | --- |
| **不用任何 GUI 工具包** | 工具包的影响比语言大一个数量级 | 自绘 7.4 MiB vs GTK3 17.6 vs Qt6 27.4 vs **GTK4 119.3** MiB |
| 用 C 而不是 Rust/Python | Rust 只多 0.4 MiB（不值得多一套工具链）；Python 多 19.5 MiB | C 14.1 vs Rust 14.5 MiB |
| 文字用 pango（不是 cairo 自带 toy API） | 与 GTK 同一套渲染栈 ⇒ 中文/图标/字体回退免费 | 只给拉丁字体族画中文，换 CJK 字体仅差 112/98000 像素 |
| 认证用一个 **setuid 助手**而不是整个程序 setuid | 特权面最小（120 行，只验密码） | — |
| 认证用**专用 PAM 服务**，不复用 `login` | `login` 带 faillock ⇒ 会连坐 autologin（实测把机器锁在 greeter） | 坑档 `2026-10-18-lockscreen-auth-reusing-login-pam-stack-*` |
| 面板 `override-redirect` + 定期 `XRaiseWindow` | WM 不仲裁这类窗口，polybar 后映射会压在上面 | 坑档 `2026-10-18-fullscreen-override-redirect-overlay-traps` |
| 键盘 grab「显示时抓、隐藏时放」 | 否则回桌面后桌面收不到按键 | 同上 |

---

## 三·五、第四轮做完的事：**无卡片紧凑版式 + 进程榜重做**

用户口径（原话）：「**不要卡片式设计**：从视觉角度来讲，我不想让它是一个卡片一个卡片这样组成的。
你要什么内容就直接显示在上面」「**布局更紧凑**：不要让太多的标题占用太多空间」
「**优化进程展示**：(a) 增加展示数量（十几二十个，甚至二三十个）(b) 一列一列的样子，
按内存从高到低排列下来 (c) 增加实时刷新……权衡一下体验感和资源占用」
「我鼓励你分多个文件去写，去装载不同的 UI 样式」。

| 项 | 做法 |
| --- | --- |
| 无卡片 | 删掉全部 `v_panel_box`；分区只靠 **1px 细线 + 字号层级 + 留白**；面板与登录页共用同一套 `v_bg()` 渐变底（顺带解决「面板平涂 vs 登录页渐变不统一」） |
| 紧凑 | 顶栏压成一行；区块标题不单独占行（label 与值同排、右对齐）；系统区由「卡片式 3 行」改成**三栏并排**（CPU 42% / 内存 29% / 网络 29%），总高 210 逻辑像素 |
| 进程榜 | `TOPN` 6 → **40**（4 列 × 10 行，逐列向下、按 RSS 降序）；每 3 秒刷新（采样 1 Hz）；行内 `名次·名·pid·内存`，口径标注 RSS |
| 拆文件 | `view.c`（基元）/ `view_panel.c`（面板）/ `view_login.c`（登录页），删掉 `view_draw.c` |
| 曲线 | 量程量化（`v_nice_ceil`）⇒ 不再整体跳；**撤掉「预填」**（那是伪造历史）；加零基线 |
| 每核柱 | 带轨道 + 最低 2px 亮条 ⇒ 空闲核也看得见（原来只有 1px 亮条） |
| 登录页 | 去掉外层卡片；认证中加不确定态进度条 |

**本轮验收**（真机 `:0`）：`>30% 像素变化` 帧 **0/50**（不闪）；CPU **0.60% of one core**；
进程榜 40 条、列对齐逐列量过（名次/名称/pid/内存右边界误差 ≤2px）；tty7 / `slick-greeter`=0 / 单实例。

⚠️ **本轮两个自伤（都已归档 kb）**：
① `const double cw2[3] = {a, b, 0}` ⇒ 网络栏宽度 0 ⇒ sparkline 压成一根竖线、右对齐文字挤到左边缘，
**编译零告警**（判据：按行取色块游程量几何）；
② 用「启动瞬间的采样值」预填曲线 ⇒ 那个值常是 0 / 偶尔是突发尖峰 ⇒ 曲线贴底或被假尖峰压扁真数据。

## 三·六、第五轮做完的事：**立为独立项目（搬进 `/data/dev` + 建 GitHub 仓库 + 交接文档）**

用户口径：「接下来把这个变成一个项目去维护：① 新建一个项目，新建 github 的仓库，然后推送上去。
② 把该交接的文档都交接给专门负责这个项目的 AI…… 但肯定还是要体现我们系统（运维中心主机）的偏好。
就是告诉外界，这是我的方案，并不是说你能直接拿过去就能用的。」

| 项 | 做法 |
| --- | --- |
| **实体** | `hpcore:/data/dev/hpopi5b-panel`（开发域实体根；git bundle 搬全历史，非 squash）· 部署副本 `hphelm:/data/dev/hpopi5b-panel`（2026-10-19 起） |
| **仓库** | **`https://github.com/zeroicey/hpopi5b-panel`**（public、无 license，与其余 40 个仓库一致）；描述里就写明「不是通用产品」 |
| **README** | 重写：公开定位 + 实测选型表 + **「这不是给谁用的」适配前提表**（屏幕/桌面栈/认证/会话入口/红线/字体/数据源/机队口径）+ 文档地图 + 状态与边界 |
| **AGENTS.md** | **给专职维护 AI 的工作指南**：三条红线 · 硬约束（性能预算/不引工具包）· 工作流 · 每轮验收判据 · 设计系统要点 · 代码地图 · 不要做什么 · 交付纪律 · 环境速查 |
| **docs/** | `architecture.md`（状态机/双缓冲/采样/认证/数据源）· `design.md`（无卡片+紧凑+坐标归 theme.h）· `verify.md`（**看不到图怎么验**）· `deploy.md`（部署/判据/回滚）· `pitfalls.md`（14 条坑，每条带判据） |
| **tools/** | `measure.py`（量几何：色块游程/墨迹带/颜色 bbox）· `probe-frames`（帧差分判闪）· `deploy.sh`（部署 + 判据，`--check` 只读） |
| **文档分工** | **代码知识归本仓库，机队事实归 kb**（`kb/runbooks/hphelm.md` 是机队视角的权威；本仓库只负责代码与设计） |

⚠️ **「体现我们系统的偏好」是怎么落地的**：README 里那张适配表把 8 项前提（1920×1080/1.48 缩放、
lightdm+bspwm+picom、专用 PAM 服务、会话包装器、三条运维红线、Nerd Font、k10temp/nvme 设备路径、机队看板口径）
逐项列出来，并在开头用 TL;DR 写明 **“This is not a drop-in product.”**
⇒ 外界能看懂「自绘、无工具包、低占用」这套**方法**可拿走，但「这份代码」要付适配成本。

## 三·七、第六轮做完的事：**交接 + 部署链本土化（2026-10-21）**

用户口径：「给 hphelm panel 交接如何部署更新项目屏保」「他们开发完也知道如何进行更新系统屏保不用麻烦你了」。

| 项 | 做法 | 判据 |
| --- | --- | --- |
| **部署链本土化** | `tools/deploy.sh` 重写：**去掉 rsync 与 `ssh hphelm`**，改成七阶段原地部署（自检 → 构建 → 备份旧二进制 → 安装+校验 → `--dump` 语义自检 → 重启并确认 pid 变化 → 判据）。新增 `--check` / `--no-restart` | 本机三次真跑全过：VT=tty7 · greeter=0 · 面板=1 · 源码⇄已装逐字节一致 · 助手 `4755 root:root` · `sleep_off=off` · 帧差分 `0` 帧 >30% |
| **回滚有凭据了** | 安装前若内容不同 ⇒ 自动留 `/usr/local/bin/hpopi5b-panel.prev` | 文档给出「三层回滚」（二进制 / 系统文件 / 退回 lightdm greeter） |
| **`docs/deploy.md` 重写** | 从「rsync 到另一台机」改成 **「更新系统屏保」runbook**：屏保到底是什么 · 文件清单 · 开发循环 · 七阶段 · 判据表 · 三层回滚 · 六件绝不能做的事 · 交付清单 | 全文 10 节，命令均可直接粘贴 |
| **项目记忆库 `.ai/`** | engram v0.5.0（当时的本地构建 `/data/dev/engram/build/src/cli.js`；**该路径已不存在**，0.6 起用 `npx -y @zeroicey/engram@0.6`）`init --root /data/dev/hpopi5b-panel --notes "…"` ⇒ `.ai/` + `.engram/` + `.agents/skills`，契约追加进 `AGENTS.md`（原有正文保留） | `engram doctor` / `engram dump` 通过；`.ai/CURRENT_TASK.md` · `ARCHITECTURE.md` · `runbooks/update-screensaver.md` · `decisions/` · `sessions/` 已填真实内容 |
| **文档纠偏** | `AGENTS.md` §0/§4/§8/§9 与本文 §一：删掉「源码在 hpcore、部署到 hphelm」的旧事实；新增「接手第一小时读什么」 | — |

**本轮我自己在 deploy.sh 第一版里踩的三个坑（都不报错，都已修）：**

1. **`pgrep -qx` 不存在** —— procps 的 pgrep 没有 `-q` 选项，它报错并返回非 0 ⇒ 脚本却当成
   「lightdm 没在跑」发了假告警。修法：`pgrep -x lightdm >/dev/null 2>&1`。
2. **对 `/tmp/hpopi5b-panel.log` 全文件 grep 「连续 3 次」** ⇒ 2026-10-18 那两次事故的字符串一直
   躺在日志里 ⇒ **每次部署都报「面板在反复崩」**。修法：只取**最近一条 `启动：` 之后**的日志块。
3. **CPU 窗口包含了刚启动的几秒**（启动帧全额重绘 + 首次扫进程）⇒ 读出 1.60%，而稳态是 1.1~1.4%。
   修法：先热身 3 秒再开 15 秒窗口。

**⚠️ 一个待查的真问题**：CPU 实测 **1.13%–1.60%**，高于文档记录 0.60%（2026-10-18）。
其余判据全绿（不闪、不空转、几何对、单实例）。清楚到代码里的假设**不要凭猜** ——
正解是用 `HPOPI5B_PANEL_DEBUG=1` 看 it/s 与各阶段耗时，先把「重绘次数」与「单帧耗时」分开。

**⚠️ 一个未解的交付缺口（用户裁决「先不管」）**：hphelm 的 GitHub 身份**只够推 `dotfiles`**（deploy key 一仓一把），
所以本机 `git push origin main` 会权限失败。本轮提交只落在本地；**用户 2026-10-21 裁决：先不管，提交留本地** ——
不要为了这个去改仓库配置或另建密钥。哪天需要推时，要么另找有账号级密钥的机器，要么在本仓加 write deploy key。

## 三·八、第七轮做完的事：**双主机看板 + 三列版式（2026-10-21）**

用户口径（原话）：「增加生产服务器监控……在一块屏幕上同时显示本机跟生产服务器的资源监控」
「进程排行榜只占一列」「页面展示：当前主机跟生产服务器都在同一页，一人一半」
「要不要做成轮播？」→ **裁决：不要轮播**（两台常驻同屏）。

| 项 | 做法 | 判据 |
| --- | --- | --- |
| **生产服务器采集** | `src/remote.[ch]`：常驻一条 `ssh -T hpcore 'perl -'`，探针脚本 `tools/probe-remote.pl` 从 **stdin** 喂进去（**不落盘到 hpcore、不装包、不起服务、不开端口**），1 Hz 打 `S`/`C`/`P`/`E` 行 | 远端 `perl` **0.3% of one core**、本机 `ssh` <0.1%；`--dump` 12 秒内拿到整帧 |
| **不引入线程** | 子进程 stdout 直接进主循环的 `poll`（第二个 fd） | `poll[可读 0/超时 4]`、事件 0、it/s 5 |
| **只在面板态跑** | 回桌面/登录页 `remote_stop()`，但**保留已采到的快照** ⇒ 回面板立刻有整帧 | 桌面态 `ps --ppid` 下没有 ssh 子进程 |
| **半帧不提交** | `S`/`C`/`P` 写 `stg`，收到 `E` 才整体提交 | 断线时显示的是**上一整帧 + 离线 Ns**，不是半新半旧 |
| **退避重连** | 2→4→8→16→30 秒封顶；离线时列标题旁写「离线 Ns」 | 远端 `pkill perl` ⇒ 5 秒内列变黄、>60 秒变红，恢复后自动接上 |
| **三列版式** | `本机 │ 生产 │ 进程榜`；两台用**同一个 `draw_host()`** 画（版面/口径必然对称）；进程榜一列两段（各 12 条） | 量像素：两列各 648px、间距 38px、进程榜 24 行（12+12）、行距 24 |
| **配置** | `/etc/hpopi5b-panel.conf` 的 `remote=`；优先级 `--remote` > `$HPOPI5B_REMOTE` > 文件 | 改主机名不用编辑 root 的会话包装器 |
| **部署脚本** | 安装 `probe-remote.pl` 到 `/usr/local/share/hpopi5b-panel/`（644，非可执行）+ 校验；`HPOPI5B_REMOTE=…` 时顺带验链路 | `tools/deploy.sh` 真机跑通，七阶段全过 |

### 本轮的性能工作（从「文档 0.60%」追到「实测 1.0~1.4%」）

`--dump` 之外还做了三件事，**都写进了 `docs/design.md` 六·五**：

| 大头 | 证据 | 修法 |
| --- | --- | --- |
| 右对齐是「量一遍 + 排一遍」⇒ pango 整形翻倍 | 一帧 100+ 次整形 | `v_text_right_w()` 复用同一次排版；`v_char_w()` 按字符宽估算截断 |
| 整屏渐变每帧重算 | 换成纯色后单帧 11ms → 5~9ms | 渐变渲染进 `bgpix`，每帧一次 `XCopyArea` |
| 进程名逐字符测量 | 每删一个字符量一次宽度 | 等宽字体 ⇒ 量一次「MMMMMMMMMM」除 10 |

**新的 CPU 口径（务必按这个说法引用）**：
- 只本机：**约 1.0% of one core**（旧版同版式对照 1.15~2.2%，随机器负载漂移）
- 含生产服务器：**约 1.4% of one core**
- ⚠️ 文档里旧的 0.60% 是「只有一台主机、进程榜 40 条」时的数字，**与现在不可直接比较**。

### ⚠️ 本轮我自己的两个失误（都已归档）

1. **在别人满载的机器上量 CPU**：这台机当时在跑无关的 `go install`（`load average: 22.99`），
   我据此做了三轮「优化」，A/B 对照表自相矛盾（旧版比新版还慢、同版本两轮差一倍）却没看出来。
   ⇒ 新坑档 `.ai/pitfalls/cases/2026-10-21-benchmarking-cpu-under-foreign-load.md`
2. **又一次用了 `pkill -f`**：它匹配到执行它的那个 shell 自己，命令直接 143 退出。
   文档里已经写了四次，这是第五次。⇒ 测试实例一律按 `/proc/<pid>/environ` 里的 `DISPLAY` 挑 pid。

另外三列版式踩了三个**编译零告警的静默错版**（相对/绝对坐标混用、`-1` 兼职两种语义、
UTF-8 截断切半）⇒ `.ai/pitfalls/cases/2026-10-21-three-column-layout-silent-misrenders.md`

## 三·九、第七轮补丁：**版面精细化（用户看了真机后提的三条）**

用户口径：「小绿点的圆心跟文字的中心不在一条线上，偏上了」「内存占用的那条线跟数字的下方重叠」
「设备名跟下面的 IP 地址、'是否在线'离得有点近了」「标题能不能去掉，尽量让文字部分只占一行」
「进度条/图表压缩一点，别留废话空间」「竖着看还挺空，放一些别的资源占用」。

| 项 | 根因 | 修法 | 判据 |
| --- | --- | --- | --- |
| 小绿点偏上 | 用常数 `y + 8` 定位，而字体的 ascent 不随字号线性变化（CJK 与拉丁下伸部也不同） | `v_text_right_ex()` 在**同一次排版**里取墨迹 extents，把中线返回；`v_dot_at_text()` 按中线画 | 圆点 bbox 中心 == 同一行文字墨迹中心：两列都是 `109.0 == 109.0` |
| 内存条压住数字 | 绝对坐标：26pt 数字占 342..378，条写死在 376 | 改成流式排版：行 24 → 条 10 → 间距 9 | 文字墨迹 441..466、条 472..487，间隔 6px |
| 设备名/在线状态太挤 | 主机名行高 30，而 17pt 文字占 26 | `L_H_NAME_H` 30 → **36** | 主机名墨迹 96..121、身份行 145..161，间距 23px（原 5px） |
| 标题占一整行 | 每块有独立标题行 | 标题与数值**同排**（`CPU 13% 58°C …`），省掉 4 行 | 每块最多一行文字 + 一条进度条 |
| 版面底部空一片 | 曲线固定 46px，剩下的高度没人用 | 四张曲线（CPU/内存/网络/温度）**均分剩余高度** | `LAYOUT_DEBUG=1` ⇒ `余=0.0` |
| 「竖着看还挺空」 | 只有 3 个旧指标 | 新增：**可回收缓存 · swap · 累计流量 · 丢包/错包 · PSI 压力(cpu/io) · 线程数 · 温度曲线** | 远端探针 `S` 行**末尾追加** 9 个字段（老探针仍可用） |

**又踩一个静默错版**：`fixed`（固定高度表）比绘制顺序**多算了一块 43px** ⇒ 曲线被压小、
底部空一片，而编译零告警、帧差分也正常。修法是把 `fixed` 的每一项按绘制顺序编号注释，
**判据写成 `LAYOUT_DEBUG=1` 的「余」必须 = 0.0**（负数 = 已超出屏幕底部）。

## 三·十、紧急修复：**面板显示 hpcore 离线（2026-10-21 用户报障）**

用户：「hpcore 怎么离线了哥们」。排查结论：**hpcore 完全正常**（ping 通、ssh 通、load 0.15），
是面板自己的采集链卡死了。

| 根因 | 说明 |
| --- | --- |
| **「连上了但没数据」永不重连** | `remote_reap()` 只在 `saw_frame && age > 30` 时掐子进程。而「ssh 连上了、远端 perl 却没起来」（对方重启/perl 挂了）**永远进不了这个分支** ⇒ 既不重连也不报错 ⇒ 永久离线，只有重启面板才恢复 |
| **运行态零日志** | 远程采集只在 `--dump` 里输出，运行态的断开/重连**一声不响** ⇒ 当时只能靠猜，排查花了半小时 |

修法：
- 超时判据覆盖 `saw_frame == 0`：**从未收到帧 ⇒ 连接存活 20 秒即失败**；收到过帧 ⇒ 30 秒无新帧失败
- 加 `rlog()`：拉起 / 已连上 / EOF / 进程退出 / 超时重连，**每次变迁一行**（正常一天几行）

判据（真机 + 假 ssh 双验）：
- 假 ssh（连上但永不输出）⇒ 20 秒内出现「连接后 20s 无数据 ⇒ 掐掉重连」→「拉起 ssh（第 2 次尝试）」
- 真机杀远端 perl ⇒ `ssh 已退出（收到 EOF）` → 立刻 `拉起 ssh` → 2 秒内 `已连上（16 核，391 个进程）`
- 屏幕：右列绿点回来（126 px 绿色像素）、`VT=tty7 · greeter=0 · panel=1 · 帧差分 0`

## 四、已知粗糙点（下一轮的候选）

**UI / 视觉**（第四轮已解决 1/2/3/7 的一部分：顶栏自己画了时间与状态、每核柱带轨道、
曲线不预填但有零基线、两界面统一渐变底；第七轮解决 4 的一半、新增双主机）
1. ~~面板顶部 48px 的空白~~ ⇒ 顶栏自己画了主机/IP/uptime/核数 + 时间 + 24×7 状态点。
2. ~~每核小柱看不清~~ ⇒ 带轨道 + 最低 2px 亮条。
3. ~~sparkline 起步为空~~ ⇒ 不预填（诚实生长）+ 零基线；若要「一开机就铺满」，正解是**缩短窗口**
   （比如只画最近 120 个样本 = 2 分钟），而不是伪造数据。
4. **进程榜 RSS 与 PSS 口径不一致**：榜单是 RSS（和 htop 一致），而模块标题写「按内存排序」。
   可以标注口径，或改用 PSS（贵）。
5. **登录页卡片位置**：现在卡片中心在 `cy+40`，时钟在上方；整体偏「上下分布」，可以再收拢。
6. **没有错误态以外的反馈**：认证中只有「正在验证…」，可以加一个转圈或进度条。
7. **颜色**：`C_BASE` 大面积填充 `#1e1e2e`，与登录页的渐变风格不统一（面板是平涂，登录页是渐变）。

**功能 / 其他**
8. **「立即锁屏」只有信号**：`kill -USR1`。可以在 `sxhkd` 里包一个快捷键（`pkill -USR1 -x hpopi5b-panel`）。
9. ~~**DPMS 关屏后唤不醒**：`sleep_off=3600` 后关屏，理论上任意输入会 `dpms(1)`；
   但实测未验证（需要等 1 小时）。可以调小 `--sleep-off` 测一次。~~
   **2026-10-19 关闭**：用户裁决定位是「看板要 7×24 一直显示」⇒ 默认 `sleep_off=0`（永不关屏），
   并且把「0 语义」写对了（`CFG_SLEEP_OFF > 0 && idle >= ...`；旧代码裸比较会让 0 变成「立即关屏」）。
   唤醒路径（`on_key/on_motion/on_button` 里的 `if (screen_off) dpms(1)`）保留不动，以备日后调回正数。
10. ~~**面板内容还没「服务概览」**~~ ⇒ 第七轮做了**一半**：显示「容器 N 个」（数 cgroup 里的
    `docker-*.scope`，不 fork docker）。**还没做的**：容器**健康**（是否在重启循环）、
    备份是否成功、证书到期——这些要读 docker API 或探针加字段。
    **2026-11-06 补**：hpcore 上 `happpy` **不在 docker 组**（`docker ps` 报 permission denied）
    ⇒ 容器**名字**拿不到，只能数个数。要做服务名清单得先给权限，或在探针里加一份 `docker ps` 缓存。
11. **多显示器未处理**：现在固定用 `DefaultScreen` 的整屏尺寸。
    第七轮的三列版式是**按 1297×729 逻辑像素**调的 —— 换到 1080p 以下的屏会挤（要重调 `L_*`）。
12. ~~**CPU 0.8%**：主要花在每秒约 50 段 pango 文字整形。两个杠杆：PangoLayout 缓存、区域局部重绘。~~
    **2026-10-18 重测：CPU 0.60% of one core**（1 Hz 采样；绘制 ~3.5ms/帧、上屏 ~0.05ms、签名 ~0.05ms）
    ⇒ 这一条优先级已降。真要再降就先看 `HPOPI5B_PANEL_DEBUG=1` 的耗时分布。
    **2026-11-06 更正**：那个 0.60% 是 hphelm 上的数，在 hpopi5b 上**不可复现**（`ondemand` 空载
    408 MHz）。同条件 A/B：旧三列 3.47% / 新单机 3.68%。**再降 CPU 的正解还是 PangoLayout 缓存**
    （每秒 ~50 段文字 + 24 行进程榜 = 主要成本），以及「值没变就不重画那一行」的区域局部重绘。
13. **面板只在 :0 上**：换接口（DP-2→HDMI）后尺寸变化由 `ConfigureNotify` 处理，但没实测过。

---

## 四·五、本轮（第二轮）做完的事：**消闪 + 消除自激空转**

| # | 问题 | 根因 | 修法 | 判据 |
| --- | --- | --- | --- | --- |
| 1 | **面板每秒闪一下**（用户：「我这个屏幕现在会有一点闪……就是内存、网络、CPU 那个界面」） | 直接往窗口上画 = **没有双缓冲**：每帧先 `cairo_paint(C_BASE)` 清底、再逐块画，cairo 分批下发 ⇒ 中间态被 picom/屏幕看到 | 绘制全进**离屏 `Pixmap`**，每帧**一次 `XCopyArea`** 上屏；`XSetWindowBackgroundPixmap` 让服务器回填；`win_show()` 先画后映射；`ConfigureNotify` 重建 pixmap | 抓窗口连续帧差分：`>30% 像素变化` 帧 **4/40 → 0**；静止时 37/39 帧 `diffpx=0` |
| 2 | **面板 CPU% 跳成 KTV**（用户：「哥们我 cpu 在唱 k 啊 疯狂跳」） | ⚠️ **我自己引入的**：`XCreateGC(dpy,win,0,NULL)` 的 `graphics_exposures` 默认 True ⇒ 每次 `XCopyArea` 回一个 `NoExpose` ⇒ `poll()` 立刻返回 ⇒ 主循环自激空转 35 it/s ⇒ 采样窗口被压到 ~28ms | `gcv.graphics_exposures=False` + `XCreateGC(..., GCGraphicsExposures, &gcv)`；**另加两道与循环速率解耦的保险**：采样按时间节流 1 Hz、`dt` 改单调时钟亚秒、IP/进程榜按时间刷新 | 仪表 `poll[可读 0/超时 2]`、事件 0、内容 1 次/秒、CPU **49% → 0.60%** |

**新增的两个能力**（下一轮直接用）：

```bash
HPOPI5B_PANEL_DEBUG=1 ./hpopi5b-panel --state panel   # 主循环仪表：it/s · poll 结果 · 事件类型 · 重绘/采样 · 各阶段耗时
./hpopi5b-panel --lock /tmp/x.lock ...               # 单实例锁可换 ⇒ Xvfb 测试不必杀真机实例
```

**本轮踩到的两条新坑（已归档 kb）**：

- `pitfalls/2026-10-18-panel-flicker-direct-draw-no-double-buffer.md`
- `pitfalls/2026-10-18-xcopyarea-noexpose-poll-self-sustaining-loop.md`

⚠️ **本轮操作失误（记下来）**：我在 Xvfb 测试时用 `pkill -x hpopi5b-panel`，
它把**真机上正在跑的那个实例也杀了**（同名进程）⇒ 用户屏幕上的面板重启了两次
（守护循环按「运行时长」判定 ⇒ `fails` 没被误触发、greeter 也没被招回来，红线 2 生效）。
⇒ **测试实例与真机实例要能区分**：现在用 `--lock` 分开锁，杀的时候要挑 pid（按 `/proc/<pid>/environ` 里的 `DISPLAY` 认）。

## 五、诊断入口（下一轮排障直接用）

> `AGENTS.md` 第 4 节是精简版验收清单；这里是与 hphelm 这台机器绑定的具体命令。

```bash
# 状态机日志（每次变迁都有时间戳）
tail -20 /tmp/hpopi5b-panel.log

# 关键判据
cat /sys/class/tty/tty0/active                  # tty7=会话 / tty8=lightdm greeter（说明被抢了）
pgrep -cx slick-greeter                         # 应为 0
pgrep -c -f /usr/local/bin/hpopi5b-panel         # 应为 1（多了说明单实例锁失效）
xdotool search --name hpopi5b-panel | wc -l      # 应为 1

# 顶部 48px 是否有墨迹（0 = 面板盖住了 polybar）
import -window root /tmp/w.png
magick /tmp/w.png -crop 1920x48+0+0 +repage -colorspace gray -threshold 40% -format '%[fx:mean*w*h]' info:

# grab 是否已释放（回桌面后应 GrabSuccess）
cat > /tmp/tg.c <<'C'
#include <X11/Xlib.h>
#include <stdio.h>
int main(void){Display*d=XOpenDisplay(NULL);Window w=DefaultRootWindow(d);
int r=XGrabKeyboard(d,w,True,GrabModeAsync,GrabModeAsync,CurrentTime);
printf("trygrab=%d (%s)\n",r,r==GrabSuccess?"成功=桌面能收按键":"AlreadyGrabbed=被抓着");
XUngrabKeyboard(d,CurrentTime);XCloseDisplay(d);return 0;}
C
gcc -O2 -o /tmp/tg /tmp/tg.c -lX11 && DISPLAY=:0 XAUTHORITY=~/.Xauthority /tmp/tg

# 账号有没有被 faillock 锁（会让 autologin 失败 ⇒ VT 被 greeter 抢）
sudo faillock --user oicey
sudo grep -aE "autologin|Switching to greeter" /var/log/lightdm/lightdm.log | tail -3
```

⚠️ **注意 `/tmp` 是易失的**：我用来验证渲染的 `ascii2.py` / `fine.py` 现在只在 hphelm 的 `/tmp` 里。
下一轮如果要用，先确认还在；不在的话照「一」里的方法重写（各 ~20 行 python）。

---

## 六、回滚

```bash
# 只回滚会话入口（回到「lightdm greeter 当锁屏」）
sudo cp -a /usr/local/bin/hphelm-session.bak-prepanel /usr/local/bin/hphelm-session
cp -a ~/.config/sxhkd/sxhkdrc.bak-prepanel ~/.config/sxhkd/sxhkdrc
sudo /usr/local/bin/hphelm-lockd >/dev/null 2>&1 &   # 旧的空闲锁屏守护（二进制仍在）
pkill -f /usr/local/bin/hpopi5b-panel
sudo systemctl restart lightdm
```

**整机级归档**：`/data/backups/hpopi5b-panel-20261018/`
（`panel2.bundle` 完整 git 历史 + 四处系统配置 + `RESTORE.md` 恢复清单）

⚠️ 回滚时**也要停掉 VT 守卫**，否则它会一直把 VT 往 tty7 拽：
`sudo systemctl disable --now hphelm-vt-guard.service`

---

---

## 八、⛔ 两条红线（下一轮务必先读，我在这一轮**踩了两次**，都是让用户看到本该消失的旧登录页）

### 红线 1：永远不要给 lightdm 发锁请求

面板是全屏覆盖层，**只在活跃 VT 上可见**。只要有人把锁请求发给 lightdm
（`dm-tool lock` / `light-locker` / autologin 失败），lightdm 就会**起 greeter 并把 VT 切到 tty8**
⇒ 用户看到 DM 的旧登录页，我们的面板被藏在后面。

⇒ **`hphelm-session` 里永远不出现 `dm-tool`；守护循环的兜底只允许 `xset dpms force off` + 记日志。**

### 红线 2：守护循环只把「运行 <5 秒即退出」当失败

`pkill` 的默认信号是 SIGTERM ⇒ 退出码 **143**。用「非 0 即失败」判定的话，
**每次调试重启都会给失败计数 +1**，攒到 3 次就会触发兜底 —— 我这次就是这么把 greeter 招回来的。
**判据用运行时长，不要用退出码。**

### 独立防线：VT 守卫（2026-10-18 新增）

| 项 | 值 |
| --- | --- |
| 脚本 | `/usr/local/sbin/hphelm-vt-guard` |
| 单元 | `/etc/systemd/system/hphelm-vt-guard.service`（enabled，常驻 ~1.2 MiB） |
| 动作 | **活跃 VT ≥ 8 且 `slick-greeter` 在跑 且 `bspwm` 在跑 ⇒ `chvt 7`** |
| 边界为何这么窄 | 为了**不干扰 `Ctrl+Alt+F2` 进救援 TTY**（tty2~tty6 永不介入） |

**它的实测判据**：

```sh
sudo chvt 2; sleep 3; cat /sys/class/tty/tty0/active   # 应仍是 tty2（守卫不动它）
sudo chvt 7
dm-tool lock; sleep 3; cat /sys/class/tty/tty0/active  # 应已回到 tty7
journalctl -t hphelm-vt-guard -n 3                     # 应看到「抢到 tty8 ⇒ 切回 tty7」
```

（实测：模拟 `dm-tool lock` 后 **3 秒内 VT 已回 tty7**。）

### 另外两个同类自伤（都已修）

- **日志按大小轮转**：包装器在会话启动前检查 `/tmp/hpopi5b-panel.log` > 1 MiB 就 `tail -c 256K`。
  （背景：我用 `rm -f /tmp/*.log` 清理时**把面板正在写的日志也删了** —— 进程仍持 fd，
  继续往已删除 inode 写，日志从此不可见且空间不释放。**删正在写的日志不报错，只会让日志「看起来空了」**。）
- **`xset dpms 0 0 0`**：DPMS 定时关屏交给面板自己管（它才是「空闲显示」的唯一负责人），
  否则 X 会在 11 分钟把面板涂黑。

### 相关坑档（kb）

- `2026-10-18-guard-fallback-self-inflicted-traps.md` ← **本轮最重要的教训**
- `2026-10-18-lockscreen-auth-reusing-login-pam-stack-locks-out-autologin.md`
- `2026-10-18-fullscreen-override-redirect-overlay-traps.md`

---

## 九、下一轮开工前请先读（2026-10-21 更新）

- `.ai/CURRENT_TASK.md`（此刻在做什么 / 卡在哪 / 下一步）——`engram dump` 也能一行拿到指纹
- **`docs/deploy.md`**（更新系统屏保：七阶段、判据表、三层回滚、六件绝不能做的事）
- `README.md`（本项目自述：行为、选型实测表、安全模型、六条适配前提）
- kb `runbooks/hphelm.md` 的「运维面板（hpopi5b-panel）」节（机队视角 + 验收判据；kb 在 `hphelm:/data/kb`）
- kb 两个坑档：`2026-10-18-lockscreen-auth-reusing-login-pam-stack-*`、
  `2026-10-18-fullscreen-override-redirect-overlay-traps`
