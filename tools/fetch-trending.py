#!/usr/bin/env python3
"""fetch-trending.py —— 把 GitHub Trending 榜单抓成一行一条的 TSV

为什么是「抓网页」而不是 API：**GitHub 没有 trending 的 API**。
`/search/repositories?q=created:>…&sort=stars` 给的是「新建且涨星」，不是 Trending
（老仓库翻红它看不见）。Trending 只存在于 HTML 页面里，官方也不打算给接口。

输出格式（面板只认这个，不解析 JSON、不带依赖）：

    # github-trending daily 2026-11-05T06:12:03+00:00 16
    owner/repo\tPython\t2,050\t12,345\t描述…

用法：
    fetch-trending.py OUT.tsv [--since daily|weekly] [--language rust]

部署：systemd 的 hpopi5b-trending.timer 每 3 小时跑一次；面板只读缓存文件，
**网络故障与面板渲染互相隔离**（抓不到就保留上一份，面板照常显示）。

防「半页榜单」（2026-11-09 加）：页面结尾不是 </html>、article 条数与
「stars today」处数不一致、或条数比上一份骤降 —— 都算可疑，重抓一次；
两次都可疑就**不覆盖缓存**并以非 0 退出（journal 里能看到原因）。
两次结果一致就当真：GitHub 自己也会给出比前一天短的榜单（当天只有 9 条）。
"""
import html
import os
import re
import sys
import tempfile
import time
import unicodedata
import urllib.request

URL = "https://github.com/trending"
UA = "hpopi5b-panel trending fetcher (https://github.com/zeroicey/hpopi5b-panel)"

BLOCK = re.compile(r'<article class="Box-row">(.*?)</article>', re.S)
HREF = re.compile(r'<h2[^>]*>.*?href="/([^"/]+/[^"/]+)"', re.S)
DESC = re.compile(r'<p class="col-9[^"]*">(.*?)</p>', re.S)
LANG = re.compile(r'itemprop="programmingLanguage">([^<]+)<')
TODAY = re.compile(r'([\d,]+)\s+stars today')
TOTAL = re.compile(r'href="/[^"]+/stargazers"[^>]*>.*?</svg>\s*([\d,]+)', re.S)
TAG = re.compile(r'<[^>]+>')
WS = re.compile(r'\s+')


def clean(s: str) -> str:
    s = html.unescape(TAG.sub(" ", s))
    out = []
    for ch in s:
        if unicodedata.category(ch) in ("So", "Sk", "Cf"):  # emoji / 变体选择符 / ZWJ
            continue
        out.append(ch)
    return WS.sub(" ", "".join(out)).strip()


def fetch_page(url: str) -> str:
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=25) as r:
        return r.read().decode("utf-8", "replace")


def parse(page: str):
    """HTML → 行列表。只认 <article class="Box-row">，GitHub 改版时这里会先炸"""
    rows = []
    for b in BLOCK.findall(page):
        m = HREF.search(b)
        if not m:
            continue
        name = m.group(1)
        d = DESC.search(b)
        desc = clean(d.group(1)) if d else ""
        if len(desc) > 160:
            desc = desc[:157] + "…"
        lang = LANG.search(b)
        today = TODAY.search(b)
        total = TOTAL.search(b)
        rows.append((name, clean(lang.group(1)) if lang else "", today.group(1) if today else "0",
                     total.group(1) if total else "0", desc))
    return rows


def prev_count(path: str) -> int:
    """上一份缓存的条数（取头行末字段；没有或读不了就是 0）"""
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            first = f.readline()
    except OSError:
        return 0
    m = re.search(r"\s(\d+)\s*$", first)
    return int(m.group(1)) if m else 0


def main() -> int:
    args = sys.argv[1:]
    if not args:
        print(__doc__, file=sys.stderr)
        return 2
    out_path = args[0]
    since = "daily"
    language = ""
    for i, a in enumerate(args):
        if a == "--since" and i + 1 < len(args):
            since = args[i + 1]
        if a == "--language" and i + 1 < len(args):
            language = args[i + 1]

    url = f"{URL}?since={since}"
    if language:
        url += "&language=" + language
    # 抓取失败/疑似半页时不覆盖缓存（面板继续显示上一份）：最多试两次，
    # 第二次结果与第一次一致才当真 —— 这样既能挡住「chunked 传到一半就断」，
    # 又不会把「GitHub 今天真的只给了 9 条」当成故障一直钉着旧榜单。
    rows = None
    why = ""
    prev = prev_count(out_path)
    for attempt in (1, 2):
        try:
            page = fetch_page(url)
        except Exception as e:  # 超时 / 连接断 / IncompleteRead
            why = f"{type(e).__name__}: {e}"
        else:
            if not page.rstrip().endswith("</html>"):
                why = f"页面不完整：{len(page)} 字节，结尾没有 </html>"
            else:
                got = parse(page)
                n_today = page.count("stars today")
                if not got:
                    why = "没抓到任何条目（页面结构变了？）"
                elif len(got) != n_today:
                    why = f"article {len(got)} 条 vs stars today {n_today} 处，疑似截断"
                elif attempt == 1 and prev and len(got) < max(5, int(prev * 0.6)):
                    why = f"新抓 {len(got)} 条 < 上一份 {prev} 条 × 0.6，重抓一次确认"
                else:
                    rows = got
                    break
        if attempt == 1:
            time.sleep(5)

    if rows is None:
        print(f"trending: 抓取失败，保留上一份缓存（{prev} 条）—— {why}", file=sys.stderr)
        return 1

    if why:
        print(f"trending: 重抓一次后接受（首次的可疑原因：{why}）", file=sys.stderr)

    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=os.path.dirname(out_path) or ".", prefix=".trending.")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write(f"# github-trending {since} {time.strftime('%Y-%m-%dT%H:%M:%S%z')} {len(rows)}\n")
            for row in rows:
                f.write("\t".join(row) + "\n")
        os.replace(tmp, out_path)  # 原子替换：面板永远读到完整文件
    finally:
        if os.path.exists(tmp):
            os.unlink(tmp)

    print(f"trending: {len(rows)} 条 → {out_path} ({since}{'/' + language if language else ''})")
    return 0


if __name__ == "__main__":
    sys.exit(main())