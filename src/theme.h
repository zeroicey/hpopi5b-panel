/* theme.h —— Catppuccin Mocha + 版式常量（全部是「逻辑像素」，绘制时统一乘 SCALE）
 *
 * 版式原则（2026-10-18 第三轮定，用户口径）：
 *   ① **不用卡片**：内容直接铺在整屏渐变底上，只用「细线 + 字号层级 + 留白」分区；
 *   ② **紧凑**：区块标题不单独占行（label 与值同排、靠右对齐），字号/行距压到最小可读；
 *   ③ 版面自左向右三列（2026-10-21 第四版，用户口径「进程榜只占一列」）：
 *        本机 hphelm │ 生产 hpcore │ 进程榜（整列高）
 *      两台主机用**同一个 draw_host()** 画 —— 版面/口径必然对称，不会「一台多一行」。
 */
#ifndef THEME_H
#define THEME_H

#include <stdint.h>
#include <time.h>

/* ---- 主题：白天 / 夜间两套配色 ----
 *
 * 语义名保持不变（C_MAUVE 永远是「CPU/主色」），只是背后的色值随主题变。
 * 所有绘制代码只读 C_* 宏 ⇒ 换主题不需要动任何版面文件。
 * TP 由 theme_set() 在运行期整体赋值；bg0/bg1 是整屏渐变的两端。
 */
typedef struct {
    uint32_t base, mantle, crust, surface0, surface1, surface2;
    uint32_t text, subtext1, subtext0, overlay1, overlay0;
    uint32_t mauve, blue, green, yellow, peach, red, teal, lavender;
    uint32_t bg0, bg1;          /* 整屏渐变：bg0 在顶部，bg1 在底部 */
} palette_t;

extern palette_t TP;            /* 当前生效的主题 */
extern const palette_t THEME_DAY, THEME_NIGHT;

#define C_BASE      TP.base
#define C_MANTLE    TP.mantle
#define C_CRUST     TP.crust
#define C_SURFACE0  TP.surface0
#define C_SURFACE1  TP.surface1
#define C_SURFACE2  TP.surface2
#define C_TEXT      TP.text
#define C_SUBTEXT1  TP.subtext1
#define C_SUBTEXT0  TP.subtext0
#define C_OVERLAY1  TP.overlay1
#define C_OVERLAY0  TP.overlay0
#define C_MAUVE     TP.mauve
#define C_BLUE      TP.blue
#define C_GREEN     TP.green
#define C_YELLOW    TP.yellow
#define C_PEACH     TP.peach
#define C_RED       TP.red
#define C_TEAL      TP.teal
#define C_LAVENDER  TP.lavender
#define C_BG0       TP.bg0
#define C_BG1       TP.bg1

void theme_set(int night);                       /* 立即生效（调用方负责重画背景） */
/* 固定时刻规则：day_min 之前、night_min 之后算夜间；两个参数都是「0 点起的分钟数」 */
int theme_night_at(time_t t, int day_min, int night_min);
int theme_is_night(void);

/* ---- 版式（逻辑像素；屏 141.8dpi ⇒ SCALE≈1.48，逻辑屏 ≈1297×729） ---- */
#define L_MARGIN      18.0
#define L_COL_GAP     26.0   /* 列之间的水平间距 */

#define L_TOP_Y       12.0   /* 顶栏文字顶部 */
#define L_RULE1_Y     46.0   /* 顶栏下分隔线 */
#define L_BODY_Y      58.0   /* 主体顶部 */

/* ---- 宽度分配（2026-11-06 单机化：hpcore all-in-one 之后只剩一台机）----
 * 原来是「本机 │ 生产服务器 │ 进程榜」三列；现在 = 主机区（内含左右两个子列）+ 进程榜一列。
 * ⚠️ 占比必须 < 1；进程榜宽度用「剩余量」算 ⇒ 不会出现「宽度 0」那种静默错版。
 */
#define L_HOST_FRAC   0.660  /* 主机区占内容宽度的比例（≈832 ⇒ 两个子列各 ≈399，曲线更宽更不像「占位」） */
#define L_SUB_GAP     34.0   /* 主机区内部左右两个子列的间距 */

/* ---- 主机区内部（**自上而下的流式排版**）----
 * 2026-10-21 第五版（用户口径：标题行不单独占行、图表压缩、别留废话空间）：
 *   · 每个资源块 = **一行文字**（标题嵌在数值行里）+ 一条细进度条，没有单独的标题行
 *   · 纵向不再写死绝对坐标，而是「流」：一行一行往下走，剩下的高度**全部分给曲线**
 *     ⇒ 以后加/减监控项不会再把版面顶出屏幕（上一版就是因为绝对坐标+加内容踩过）
 * 2026-11-06 单机版：两个子列各自「2 张曲线 + 其余流式内容」，两列的固定高度**对齐**
 *   （187 + 187），所以两张曲线共用同一个高度、左右两列的横向基线自然对齐。
 *
 * 纵向预算（逻辑屏高 729.7、页边 18 ⇒ 内容区 58..711，共 653）：
 *   左列固定 187（名称 36 + 身份 20 + 细线 8 + CPU 行 24 + 条 10+9 + 每核柱 28+9
 *                   + 内存行 24 + 条 10+9）
 *   右列固定 187（网络行 24 + 磁盘 I/O 行 24 + 细线 8 + 磁盘行 24 + 条 10+9
 *                   + 细线 8 + 三行小字 24×3）
 *   ⇒ 653 - 187 - 2×6(曲线间距) = 448 ⇒ 每张曲线 224（占满，不靠假数据填）
 */
#define L_H_NAME_Y      58.0   /* 列内第一行（主机名）的顶部 */
#define L_H_NAME_H      36.0   /* 主机名行高（17pt；用户要求与下面的身份行拉开） */
#define L_H_IDENT_H     20.0   /* 身份行（角色/IP/up/核/网卡） */
#define L_H_RULE_H       8.0   /* 细线上下的留白合计 */
#define L_H_LINE_H      24.0   /* 一个资源行的行高（标题 + 数值同排） */
#define L_H_BAR_H       10.0   /* 进度条高度 */
#define L_H_BAR_GAP      9.0   /* 进度条与下一行的间距 */
#define L_H_CORES_H     28.0   /* 每核柱高度 */
#define L_H_CHART_MIN   30.0   /* 曲线最小高度 */
#define L_H_CHART_MAX  150.0   /* 曲线最大高度（2026-11-06 第二版：用户说「一个折线图占位太多」⇒ 从 260 收到 150，
                                * 腾出来的高度给容器块；没有容器时曲线才长回这个上限） */
#define L_H_CAP_H      16.0   /* 曲线时间轴说明行（「近 5 分钟 · 1 Hz」），两个子列各一条 */
#define L_H_CHART_GAP    6.0   /* 曲线与其下内容的间距 */
#define L_H_FOOT_H      18.0   /* 脚注行（容器/SSD/累计流量） */

/* ---- 尾网块（2026-11-06 第三版）：这块位置先做过「容器清单」，用户指出
 * 「容器好像不就是进程吗 那算了 不要了 换成别的吧」⇒ 换成 Tailscale：
 * 哪几台在线、走直连还是 DERP 中继、此刻在传多少。放在主机区**下面**（尾网是这台机的入口）、
 * 占满两个子列的整宽：左子列 5 台 + 右子列 5 台 = 10 台。
 * 数据来自探针的 T 行（tailscaled 的 localapi，不 fork CLI）。
 */
/* ---- GitHub 趋势幕（2026-11-06 第五版）：行高随内容走的**流式两列** ----
 * 用户口径「还是有很多空白空间，内容还能加多」。历史：5 行/列（行距 127、只用掉 72 ⇒ 一半是空）
 * → 8 行/列固定网格（15 条上屏，但 2 行描述只差 1 像素就碰到行线，**挤**）。
 * ⇒ 现在不再用固定网格：每行高度 = 名字 + 实际行数（1~2 行描述）+ 行距，两列按**总高度**均衡分列。
 * 实测 15 条（9 条两行 + 6 条一行）= 1176 + 语言块 68 ⇒ 两列各 ≈622 < 可用 635 ⇒ 全部上屏且不挤。
 * 换屏/换 scale 都不会顶出屏幕：可用高度从 lh 现算，放不下的条目不画（宁可少一条，也不压页脚）。
 */
#define L_TR_Y0        (L_BODY_Y + 10.0)  /* 第一行的顶部 */
#define L_TR_NAME_GAP  3.0                /* 仓库名与描述之间的空隙 */
#define L_TR_RULE_B    5.0                /* 行分隔线距行底 */
#define L_TR_ROW_GAP  10.0                /* 行与行之间的空隙（含行线所在的位置） */
#define L_TR_RANK_W   26.0                /* 序号列宽（序号 + 空隙） */
#define L_TR_DESC_LINES 2                 /* 描述最多几行（第 3 行开始折行省略） */
#define L_TR_LANG_H   34.0                /* 尾部「语言分布」块占的高度（一行文字 + 一条堆叠条） */
#define L_TR_LANG_TXT_H 16.0              /* 那一行文字的高度（9pt） */
#define L_TR_ROW_PAD_MAX 8.0              /* 零头均摊到行距时每行最多加多少（封顶＝不回到「大空白」） */

/* ---- 新闻幕（2026-11-08 第八版）：左「国内热点」/ 右「科技」两列固定行高 ----
 * 为什么固定行高：两条新闻都只有「标题一行」（超出按像素截断），不像 trending 有 1~2 行描述，
 * 所以不需要流式排版；固定行高让两列的行线和序号天然对齐（用户口径「两列吧」）。
 * 条数**不写死**：由可用高度现算（avail / ROW_H），放不下的不画，宁可少一条也不压页脚。
 * 纵向预算（729.7 逻辑高）：y0 = 58 + 10 = 68，列标题 26，可用 ≈ 729.7 - 26(页脚) - 94 ≈ 610
 * ⇒ 15 行/列（40×15 = 600，余 10 均摊到行距里）。
 */
/* 2026-11-09 第二版（用户口径「单调、空间利用率不够、列标题可以删掉」）：
 *   三列 = 热点 1 列 + 科技 2 列；删掉列标题行；热点首条放大；每行带第二条信息线
 *   （热点是热度条 + 数值，科技是来源 + 时间），三列之间用 1px 竖细线分区。 */
#define L_NW_Y0        (L_BODY_Y + 6.0)   /* 第一行顶部（列标题行删掉了 ⇒ 主体上提） */
#define L_NW_ROW_H     44.0               /* 一行新闻的高度（两行式：24pt 标题行 + 16pt 信息行） */
#define L_NW_LEAD_H    92.0               /* 热点列首条块高（17pt 两行标题 + 热度条） */
#define L_NW_RANK_W    26.0               /* 序号列宽（与 trending 同宽，两个幕不要打架） */
#define L_NW_AGE_W     44.0               /* 科技列右对齐时间「21h / 1d」的预留宽 */
#define L_NW_HOT_FRAC   0.34              /* 热点列在（内容宽 − 2 列间距）里占的比例 */
#define L_NW_BAR_H      2.5               /* 热度条粗细（轨道 + 游标） */
#define L_NW_BAR_W    150.0               /* 热度条满长（封顶：条子是刻度，不是下划线） */
#define L_NW_BAR_W_LEAD 210.0             /* 首条的热度条满长 */
#define L_NW_BAR_GAP   12.0               /* 热度条右端到数值的留白 */
#define L_NW_COL_PAD_MAX 8.0              /* 零头均摊到行距的封顶 */

#define L_TS_GAP       18.0   /* 主机块与尾网块之间的空隙 */
#define L_TS_TITLE_H   24.0   /* 「Tailscale 9 台 · 7 在线」这一行 */
#define L_TS_ROW_H     22.0   /* 一台设备一行 */
#define L_TS_ROWS       5     /* 每个子列几行（共 2×5 = 10 台） */
#define L_TS_PAD_B     16.0   /* 尾网块下沿留白（贴着屏幕边缘会显得被切掉） */

/* ---- 进程榜列（**一整列一条榜**：24 行按 CPU，行数由可用高度算出来）----
 * 2026-11-06 第二版：原来是「按内存 12 行 + 按 CPU 12 行」两段，用户直接说「两个一模一样的列表」
 * ⇒ 砍掉重复，一列只有一条榜。排序按 CPU%（同值比 RSS），所以空载时它自动退化成
 * 「最大的几个进程」（这也是原来想用两段表达的东西）。CPU% 与 RSS 两列都还在。
 */
#define L_PROC_ROW_H  24.0   /* 行高 */
#define L_PROC_TITLE_H 28.0  /* 标题行高（榜名 + 共 N 个 + 右对齐排序口径） */
#define L_PROC_MAX    26     /* 行数上限（再多人眼也读不过来；实际行数按可用高度算） */

#define L_CORE_BAR_GAP 4.0   /* 每核柱间距 */

/* ---- 字号 ---- */
#define F_TITLE  "JetBrainsMono Nerd Font 11"
#define F_LABEL  "JetBrainsMono Nerd Font 10"
#define F_SMALL  "JetBrainsMono Nerd Font 9"
#define F_DESC   "JetBrainsMono Nerd Font 13"  /* 榜单描述：要比 label 大、又不能抢过 repo */
#define F_PROC   "JetBrainsMono Nerd Font 14"
#define F_VALUE  "JetBrainsMono Nerd Font 17"
#define F_BIG    "JetBrainsMono Nerd Font 26"
#define F_HUGE   "JetBrainsMono Nerd Font 44"

#endif
