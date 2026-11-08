#!/usr/bin/env python3
"""fetch-news.py —— 把「国内热点 + 科技新闻」抓成一行一条的 TSV（新闻幕的数据）

为什么是这个组合（2026-11-08 实测，在 hpopi5b 上量的）：
  · 国内热点：微博热搜 403 / 知乎 401 / 澎湃与 RSSHub 不通 ⇒ **今日头条热榜**能用
    （JSON、0.46s、50 条、带热度值），退化时用百度热搜（0.26s、51 条、无热度值）。
  · 科技：Solidot（0.34s、需带 UA，裸抓 403）、少数派（0.17s）、36氪（0.30s）三家 RSS
    合并后按时间倒序 —— 单家的量撑不满一列，三家混着才像「今天发生了什么」。
  · 人民网 RSS 停在 2025-06、新华网只剩 1 条、新浪 RSS 1 条：都已退役，不要再加回来。

输出格式（面板只认这个，不解析 JSON/XML、不带依赖）：

    # news 2026-11-08T21:40:00+08:00 hot=14 tech=30
    H\t<title>\t头条\t1315万
    T\t<title>\tSolidot\t1762606349

  · 第 1 列：H = 国内热点（第 4 列是热度的人话写法），T = 科技（第 4 列是 unix 时间戳）
  · 第 3 列是来源短名，右列直接上屏；条数由面板按可用高度决定，这里只负责「给足池子」

用法：
    fetch-news.py OUT.tsv [--hot 14] [--tech 30]

部署：systemd 的 hpopi5b-news.timer 每 30 分钟跑一次；面板只读缓存文件，
**网络故障与面板渲染互相隔离**（抓不到就保留上一份，面板照常显示并标注「N 分钟前同步」）。
"""
import email.utils
import html
import json
import os
import re
import sys
import tempfile
import time
import unicodedata
import urllib.request

UA = "hpopi5b-panel news fetcher (https://github.com/zeroicey/hpopi5b-panel)"
TIMEOUT = 15

TOUTIAO = "https://www.toutiao.com/hot-event/hot-board/?origin=toutiao_pc"
BAIDU = "https://top.baidu.com/api/board?platform=wise&tab=realtime"

# (短名, RSS 地址)；加源 = 这里加一行
TECH_FEEDS = [
    ("Solidot", "https://www.solidot.org/index.rss"),
    ("少数派", "https://sspai.com/feed"),
    ("36氪", "https://36kr.com/feed"),
]

TAG = re.compile(r"<[^>]+>")
WS = re.compile(r"\s+")
ITEM = re.compile(r"<(?:item|entry)\b.*?</(?:item|entry)>", re.S)
TITLE = re.compile(r"<title[^>]*>(.*?)</title>", re.S)
CDATA = re.compile(r"<!\[CDATA\[(.*?)\]\]>", re.S)
DATE_RFC = re.compile(r"<pubDate[^>]*>(.*?)</pubDate>", re.S)
DATE_ISO = re.compile(r"<(?:dc:date|published|updated)[^>]*>(.*?)</(?:dc:date|published|updated)>", re.S)


def clean(s: str) -> str:
    """去 HTML、解实体、压空白，丢掉 emoji/变体选择符/ZWJ（pango 里它们是豆腐块）。"""
    s = CDATA.sub(r"\1", s)
    s = html.unescape(TAG.sub(" ", s))
    out = []
    for ch in s:
        if unicodedata.category(ch) in ("So", "Sk", "Cf"):
            continue
        out.append(ch)
    return WS.sub(" ", "".join(out)).strip()


def get(url: str) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": UA, "Accept": "*/*"})
    with urllib.request.urlopen(req, timeout=TIMEOUT) as r:
        return r.read()


def human_hot(v) -> str:
    """热度值 → 中文人话：13147665 ⇒ 1315万。取不到就给空串（不编数字）。"""
    try:
        n = float(v)
    except (TypeError, ValueError):
        return ""
    if n <= 0:
        return ""
    if n >= 1e8:
        return f"{n / 1e8:.1f}亿"
    if n >= 1e4:
        return f"{n / 1e4:.0f}万"
    return f"{int(n)}"


def hot_toutiao() -> list:
    d = json.loads(get(TOUTIAO).decode("utf-8", "replace"))
    out = []
    for it in d.get("data") or []:
        t = clean(it.get("Title") or "")
        if t:
            out.append((t, "头条", human_hot(it.get("HotValue"))))
    return out


def hot_baidu() -> list:
    d = json.loads(get(BAIDU).decode("utf-8", "replace"))
    cards = (d.get("data") or {}).get("cards") or []
    if not cards:
        return []
    # 结构：cards[0].content[0].content[] —— 两个 content 不是笔误
    block = (cards[0].get("content") or [{}])[0]
    out = []
    for it in block.get("content") or []:
        t = clean(it.get("word") or "")
        if t:
            out.append((t, "百度", ""))
    return out


def parse_feed(xml: str, src: str) -> list:
    out = []
    now = time.time()
    for block in ITEM.findall(xml):
        m = TITLE.search(block)
        if not m:
            continue
        title = clean(m.group(1))
        if not title:
            continue
        ts = 0.0
        for pat in (DATE_RFC, DATE_ISO):
            dm = pat.search(block)
            if not dm:
                continue
            raw = clean(dm.group(1))
            try:
                ts = email.utils.parsedate_to_datetime(raw).timestamp()
            except (TypeError, ValueError):
                try:
                    ts = time.mktime(time.strptime(raw[:19], "%Y-%m-%dT%H:%M:%S"))
                except ValueError:
                    ts = 0.0
            if ts:
                break
        if ts > now + 86400:  # 时区写错的源（未来时间）按「现在」算，别让它霸榜
            ts = now
        out.append((title, src, str(int(ts)) if ts else ""))
    return out


def dedupe(rows: list, limit: int) -> list:
    seen, out = set(), []
    for r in rows:
        k = r[0]
        if k in seen:
            continue
        seen.add(k)
        out.append(r)
        if len(out) >= limit:
            break
    return out


def main() -> int:
    args = sys.argv[1:]
    if not args:
        print(__doc__, file=sys.stderr)
        return 2
    out_path = args[0]
    cap_hot, cap_tech = 14, 30
    for i, a in enumerate(args):
        if a == "--hot" and i + 1 < len(args):
            cap_hot = max(1, int(args[i + 1]))
        if a == "--tech" and i + 1 < len(args):
            cap_tech = max(1, int(args[i + 1]))

    hot, tech, ok, fail = [], [], 0, []

    for name, fn in (("头条热榜", hot_toutiao), ("百度热搜", hot_baidu)):
        try:
            got = fn()
            if got:
                ok += 1
                hot = got
                break
            fail.append(f"{name}: 0 条")
        except Exception as e:  # 单个源坏掉不该拖垮整次抓取
            fail.append(f"{name}: {e}")

    for src, url in TECH_FEEDS:
        try:
            got = parse_feed(get(url).decode("utf-8", "replace"), src)
            if got:
                ok += 1
                tech += got
            else:
                fail.append(f"{src}: 0 条")
        except Exception as e:
            fail.append(f"{src}: {e}")

    for f in fail:
        print(f"跳过 {f}", file=sys.stderr)

    # 全挂 ⇒ 保留上一份缓存（绝不写一个空文件把好数据冲掉）
    if ok == 0:
        print("所有源都抓不到，保留上一份缓存", file=sys.stderr)
        return 1

    hot = dedupe(hot, cap_hot)
    tech.sort(key=lambda r: int(r[2] or 0), reverse=True)
    tech = dedupe(tech, cap_tech)

    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=os.path.dirname(out_path) or ".", prefix=".news.")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write(f"# news {time.strftime('%Y-%m-%dT%H:%M:%S%z')} hot={len(hot)} tech={len(tech)}\n")
            for t, src, hv in hot:
                f.write(f"H\t{t}\t{src}\t{hv}\n")
            for t, src, ts in tech:
                f.write(f"T\t{t}\t{src}\t{ts}\n")
        os.replace(tmp, out_path)  # 原子替换：面板永远读到完整文件
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise
    print(f"hot={len(hot)} tech={len(tech)} → {out_path}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())