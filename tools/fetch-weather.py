#!/usr/bin/env python3
"""fetch-weather.py —— 抓 5 城 7 天预报 + 此刻空气质量，写成面板直读的 TSV（天气幕的数据）

为什么是 Open-Meteo（2026-11-09 在 hpcore / hpopi5b 实测）：
  · 免 key、免注册；**一次请求 5 个坐标**（latitude/longitude 传逗号列表，返回是 list，
    第 i 个元素就是第 i 个城市）⇒ 5 城只花 1 次往返，46 KB / 1.6s。
  · 一次就够用：current（此刻）+ hourly（168 个整点）+ daily（7 天，含日出日落/UV）。
    整支脚本实测 3.2s（hpcore）/ 2.9s（hpopi5b, Python 3.10.12）。
  · 空气质量在**另一个 host**（air-quality-api.open-meteo.com，2 KB / 0.8s），
    所以它挂了只降级：aqi/pm2.5/uv 写 -1，主数据照常落盘。
  · 不用和风/彩云：都要注册 + key，面板没地方存密钥，也不该引入第三方依赖。

时间字段的怪癖（自己处理掉，别让 C 侧猜）：API 给的是 `2026-11-09T21:00` ——
**本地时间，但没有秒、也没有时区偏移**。头行由脚本补成 `2026-11-09T21:00:00+08:00`
（偏移量取返回里的 utc_offset_seconds，不硬编码）；H/D 行保持 API 原样（`T` 分隔）。

输出格式（面板 C 侧只 fgets + 按 TAB 切列，不解析 JSON、不带依赖）：

    # weather 2026-11-09T21:00:00+08:00 cities=5 campus=广东东软学院
    P\t广东东软学院\t佛山 · 南海 · 狮山
    C\t0\t佛山 · 南海 · 狮山\t17.7\t14.1\t53\t0\t20.6\t20\t40.7\t0.0\t1\t1028.5\t22.1\t15.5\t4\t58\t24.9\t0.0
    H\t0\t2026-11-09T21:00\t17.7\t14.1\t0\t0.0\t0\t20.6
    D\t0\t2026-11-09\t3\t22.1\t15.5\t4\t0.0\t06:37\t17:45\t6.2

  行数/列数是硬约定（C 侧按行数发现「哪段没准备好」、按列数判断整行是否可用，
  塌一列整行会被跳过）：
    P = 1 行 · C = 每城 1 行（5） · H = 每城 24 行（120） · D = 每城 7 行（35）
  H =「从 current.time 的那个整点起、接下来 24 小时」：API 的 hourly 从当天 00:00 开始，
  脚本自己按时间戳定位窗口（不靠下标），跨天、数组中缺某个整点都能落对行。
  数字格式：温度/体感/风速/阵风/气压/降水/PM2.5/UV 保留 1 位小数；湿度/云量/降水概率/
  AQI/weather_code 写整数；风向写整数度（API 给的就是整数，0~360）；空值一律 `-1`。
  weather_code 是 WMO 原码，中文映射由 C 侧做。

用法：
    fetch-weather.py [--out PATH] [--api-base URL]
    默认 out=/var/cache/hpopi5b-panel/weather.tsv。--api-base 只替换预报接口的 base
    （失败路径自测用；空气质量固定真实地址）。

行为（和 tools/fetch-news.py、fetch-trending.py 一致）：
  · 原子写（同目录 mkstemp + os.replace），面板永远读到完整文件；
  · 预报失败（HTTP/超时/解析/城市数对不上）⇒ **绝不覆盖旧缓存**，stderr 说明原因，退出码 1；
  · 空气质量失败 ⇒ 照常写主数据（aqi/pm2.5/uv=-1），stderr 警告，退出码 0；
  · 写盘前自检行数 + 每行 TAB 数，不对就不覆盖（防「整段静默塌掉」）。
  · 目标机 hpopi5b 是 Python 3.10：只用 3.10 能跑的语法与标准库。

部署：systemd 的 hpopi5b-weather.timer 定时跑；面板只读缓存文件，
**网络故障与面板渲染互相隔离**（抓不到就保留上一份，面板照常显示并标注同步时间）。
"""
import datetime as dt
import json
import os
import sys
import tempfile
import time
import urllib.parse
import urllib.request

CAMPUS = "广东东软学院"

# (城市 id, 显示名, 纬度, 经度) —— 坐标 WGS84；id 0 = 面板所在地（广东东软学院 / 狮山大学城）。
# 加城市 = 这里加一行（C 侧按 id 对应，不要改已有 id）。
CITIES = [
    (0, "佛山 · 南海 · 狮山", 23.1366, 113.0237),
    (1, "东莞 · 常平", 22.97692, 113.99024),
    (2, "湖北 · 利川", 30.29649, 108.93776),
    (3, "广州", 23.1291, 113.2644),
    (4, "深圳", 22.5431, 114.0579),
]

FORECAST_URL = "https://api.open-meteo.com/v1/forecast"
AIR_URL = "https://air-quality-api.open-meteo.com/v1/air-quality"
DEFAULT_OUT = "/var/cache/hpopi5b-panel/weather.tsv"
UA = "hpopi5b-panel weather fetcher (https://github.com/zeroicey/hpopi5b-panel)"
TIMEOUT = 20

DAYS = 7
HOURS = 24

CUR_FIELDS = ("temperature_2m,relative_humidity_2m,apparent_temperature,precipitation,"
              "weather_code,cloud_cover,pressure_msl,wind_speed_10m,wind_direction_10m,"
              "wind_gusts_10m")
HOUR_FIELDS = ("temperature_2m,apparent_temperature,precipitation_probability,"
               "precipitation,weather_code,wind_speed_10m")
DAY_FIELDS = ("weather_code,temperature_2m_max,temperature_2m_min,"
              "precipitation_probability_max,precipitation_sum,sunrise,sunset,uv_index_max")
AIR_FIELDS = "us_aqi,pm2_5,uv_index"

# 每种行的 TAB 数（= 列数 - 1）；写盘前逐行对，塌了就不覆盖缓存
TABS = {"P": 2, "C": 18, "H": 8, "D": 10}


def num(v, dec=1) -> str:
    """数值 → TSV 字段。None/空/非数/NaN/Inf 一律 '-1'（宁可露 -1 也不编数）。"""
    if v is None or isinstance(v, bool):
        return "-1"
    try:
        f = float(v)
    except (TypeError, ValueError):
        return "-1"
    if f != f or f in (float("inf"), float("-inf")):
        return "-1"
    return f"{f:.1f}" if dec else f"{int(round(f))}"


def hhmm(iso) -> str:
    """`2026-11-09T06:37` → `06:37`；切不出来给 '-1'（C 侧直接上屏）。"""
    s = str(iso or "").strip()
    i = s.find("T")
    if i < 0:
        return "-1"
    h = s[i + 1:i + 6]
    return h if len(h) == 5 and h[2] == ":" else "-1"


def hour_start(iso) -> dt.datetime:
    """`2026-11-09T21:00` → 该小时整点（脏数据直接抛，调用方决定降级还是放弃）。"""
    s = str(iso or "").strip()
    if len(s) < 16:
        raise ValueError(f"时间字段太短：{s!r}")
    return dt.datetime.strptime(s[:16], "%Y-%m-%dT%H:%M").replace(minute=0, second=0)


def iso_offset(sec) -> str:
    """28800 → '+08:00'（偏移量用 API 返回的 utc_offset_seconds，不硬编码时区）。"""
    sec = int(sec or 0)
    return ("+" if sec >= 0 else "-") + f"{abs(sec) // 3600:02d}:{abs(sec) % 3600 // 60:02d}"


def head_stamp(iso, offset_s) -> str:
    """`2026-11-09T21:00` → `2026-11-09T21:00:00+08:00`；已自带偏移就原样返回。"""
    s = str(iso or "").strip()
    for fmt in ("%Y-%m-%dT%H:%M:%S%z", "%Y-%m-%dT%H:%M%z",
                "%Y-%m-%dT%H:%M:%S", "%Y-%m-%dT%H:%M"):
        try:
            d = dt.datetime.strptime(s, fmt)
        except ValueError:
            continue
        if d.tzinfo is not None:
            return s
        return d.strftime("%Y-%m-%dT%H:%M:%S") + iso_offset(offset_s)
    return ""


def build_url(base: str, params: dict) -> str:
    q = urllib.parse.urlencode(params, safe=",")  # 逗号保留原样，坐标列表更好读
    return base + ("&" if "?" in base else "?") + q


def get_json(url: str):
    req = urllib.request.Request(url, headers={"User-Agent": UA, "Accept": "application/json"})
    with urllib.request.urlopen(req, timeout=TIMEOUT) as r:
        return json.loads(r.read().decode("utf-8", "replace"))


def latlon() -> dict:
    return {
        "latitude": ",".join(f"{c[2]:.4f}" for c in CITIES),
        "longitude": ",".join(f"{c[3]:.4f}" for c in CITIES),
        "timezone": "Asia/Shanghai",
    }


def fetch_forecast(base: str) -> list:
    """5 个坐标一次请求 → list（每个元素一个城市）。形状不对就当失败，绝不猜顺序。"""
    params = latlon()
    params.update({"forecast_days": DAYS, "current": CUR_FIELDS,
                   "hourly": HOUR_FIELDS, "daily": DAY_FIELDS})
    data = get_json(build_url(base, params))
    cities = data if isinstance(data, list) else [data]
    if len(cities) != len(CITIES):
        raise ValueError(f"请求 {len(CITIES)} 个坐标 / 返回 {len(cities)} 份 —— 城市对不上号")
    for i, one in enumerate(cities):
        if not isinstance(one, dict):
            raise ValueError(f"第 {i} 份不是对象")
        for k in ("current", "hourly", "daily", "utc_offset_seconds"):
            if k not in one:
                raise ValueError(f"第 {i} 份缺 {k}")
    return cities


def fetch_air() -> list:
    params = latlon()
    params["current"] = AIR_FIELDS
    data = get_json(build_url(AIR_URL, params))
    return data if isinstance(data, list) else [data]


def city_rows(cid: int, name: str, fc: dict, air: dict, fallback_start, warn: list):
    """一个城市的 C + H + D 行。缺字段写 -1，但**列数/行数一个都不能少**。"""
    cur = fc.get("current") or {}
    hourly = fc.get("hourly") or {}
    daily = fc.get("daily") or {}

    # 24 小时窗口从 current 的那个整点起；本城时间坏了就退回第一城的时间并警告
    try:
        start = hour_start(cur.get("time"))
    except ValueError:
        if fallback_start is None:
            raise
        start = fallback_start
        warn.append(f"城市 {cid} 的 current.time 不可用（{cur.get('time')!r}），窗口按首城时间排")

    # 今天（daily[0]）的极值 —— 按约定取 daily[0]，不按日期反查
    def today(col):
        arr = daily.get(col) or []
        return arr[0] if arr else None

    air_cur = (air or {}).get("current") or {}

    rows = []

    # C：此刻 + 今日极值 + 空气质量（AQ 挂了 air_cur 为空 ⇒ 三列 -1）
    rows.append("\t".join((
        "C", str(cid), name,
        num(cur.get("temperature_2m")),        # temp
        num(cur.get("apparent_temperature")),   # feels
        num(cur.get("relative_humidity_2m"), 0),  # rh
        num(cur.get("weather_code"), 0),        # code
        num(cur.get("wind_speed_10m")),         # wind km/h
        num(cur.get("wind_direction_10m"), 0),  # dir deg
        num(cur.get("wind_gusts_10m")),         # gust km/h
        num(cur.get("precipitation")),          # precip mm
        num(cur.get("cloud_cover"), 0),         # cloud %
        num(cur.get("pressure_msl")),           # pressure hPa
        num(today("temperature_2m_max")),       # tmax
        num(today("temperature_2m_min")),       # tmin
        num(today("precipitation_probability_max"), 0),  # pp_max
        num(air_cur.get("us_aqi"), 0),          # aqi
        num(air_cur.get("pm2_5")),              # pm2.5
        num(air_cur.get("uv_index")),           # uv now
    )))

    # H：按时间戳查 hourly（数组本身可能少整点 → 该行仍写，数值 -1）
    htime = hourly.get("time") or []
    cols = {k: hourly.get(k) or [] for k in (
        "temperature_2m", "apparent_temperature", "precipitation_probability",
        "precipitation", "weather_code", "wind_speed_10m")}
    hindex = {t: i for i, t in enumerate(htime)}

    def pick(col, i):
        arr = cols[col]
        return arr[i] if i is not None and 0 <= i < len(arr) else None

    for k in range(HOURS):
        t = start + dt.timedelta(hours=k)
        key = t.strftime("%Y-%m-%dT%H:%M")
        i = hindex.get(key)
        rows.append("\t".join((
            "H", str(cid), key,
            num(pick("temperature_2m", i)),
            num(pick("apparent_temperature", i)),
            num(pick("precipitation_probability", i), 0),
            num(pick("precipitation", i)),
            num(pick("weather_code", i), 0),
            num(pick("wind_speed_10m", i)),
        )))

    # D：7 行，缺的那天照样写日期 + 全 -1（行数不能少）
    dtime = daily.get("time") or []
    try:
        base = dt.datetime.strptime(str(dtime[0])[:10], "%Y-%m-%d").date()
    except (IndexError, ValueError):
        base = start.date()
    dindex = {str(t)[:10]: i for i, t in enumerate(dtime)}

    def d_at(col, i):
        arr = daily.get(col) or []
        return arr[i] if i is not None and 0 <= i < len(arr) else None

    for k in range(DAYS):
        day = (base + dt.timedelta(days=k)).isoformat()
        i = dindex.get(day)
        rows.append("\t".join((
            "D", str(cid), day,
            num(d_at("weather_code", i), 0),
            num(d_at("temperature_2m_max", i)),
            num(d_at("temperature_2m_min", i)),
            num(d_at("precipitation_probability_max", i), 0),
            num(d_at("precipitation_sum", i)),
            hhmm(d_at("sunrise", i)),
            hhmm(d_at("sunset", i)),
            num(d_at("uv_index_max", i)),
        )))
    return rows


def atomic_write(path: str, text: str) -> None:
    d = os.path.dirname(os.path.abspath(path))
    os.makedirs(d, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=d, prefix=".weather.")
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)  # 原子替换：面板永远读到完整文件
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


def main() -> int:
    args = sys.argv[1:]
    out_path, api_base = DEFAULT_OUT, FORECAST_URL
    i = 0
    while i < len(args):
        a = args[i]
        if a in ("-h", "--help"):
            print(__doc__)
            return 0
        if a == "--out" and i + 1 < len(args):
            out_path = args[i + 1]
            i += 2
        elif not a.startswith("-") and out_path == DEFAULT_OUT:
            # 与 fetch-news.py / fetch-trending.py 一致：支持位置参数 <out>
            out_path = a
            i += 1
        elif a == "--api-base" and i + 1 < len(args):
            api_base = args[i + 1].strip().rstrip("/")
            i += 2
        else:
            print(f"weather: 未知参数或不完整：{a}", file=sys.stderr)
            print(__doc__, file=sys.stderr)
            return 2

    t0 = time.monotonic()
    try:
        cities_fc = fetch_forecast(api_base)
    except Exception as e:  # 超时 / DNS / HTTP 4xx-5xx / JSON 坏 / 城市数对不上
        print(f"weather: 预报抓取失败，保留上一份缓存 —— {type(e).__name__}: {e}", file=sys.stderr)
        return 1

    try:
        fallback_start = hour_start(cities_fc[0]["current"]["time"])
    except Exception as e:
        print(f"weather: 预报解析失败，保留上一份缓存 —— current.time 定位不了 24 小时窗口: {e}",
              file=sys.stderr)
        return 1

    # 空气质量只是降级项：抓不到/形状不对都照常写主数据
    air_by_id = {}
    try:
        air = fetch_air()
        if len(air) != len(CITIES):
            print(f"weather: 空气质量坐标数对不上（{len(air)} 份 vs {len(CITIES)} 城），缺的写 -1",
                  file=sys.stderr)
        for idx, c in enumerate(CITIES):
            if idx < len(air) and isinstance(air[idx], dict):
                air_by_id[c[0]] = air[idx]
    except Exception as e:
        print(f"weather: 空气质量降级（aqi/pm2.5/uv 写 -1）—— {type(e).__name__}: {e}",
              file=sys.stderr)

    lines, warn = [], []
    # P：面板标题行（校区 + 主城显示名），C 侧直接拿来当标题/副标题
    lines.append("\t".join(("P", CAMPUS, CITIES[0][1])))
    n_c = n_h = n_d = 0
    for (cid, name, _lat, _lon), fc in zip(CITIES, cities_fc):
        rows = city_rows(cid, name, fc, air_by_id.get(cid), fallback_start, warn)
        for r in rows:
            tag = r[0]
            if tag == "C":
                n_c += 1
            elif tag == "H":
                n_h += 1
            else:
                n_d += 1
        lines += rows
    for w in warn:
        print(f"weather: {w}", file=sys.stderr)

    want = (len(CITIES), len(CITIES) * HOURS, len(CITIES) * DAYS)
    if (n_c, n_h, n_d) != want:
        print(f"weather: 行数不对（C {n_c} / H {n_h} / D {n_d}，期望 "
              f"{want[0]}/{want[1]}/{want[2]}），不覆盖缓存", file=sys.stderr)
        return 1
    for line in lines:
        ntab = TABS.get(line[:1])
        if ntab is None or line.count("\t") != ntab or "\r" in line or "\n" in line:
            print(f"weather: 列数/字符不对（{line[:1]} 行 TAB {line.count(chr(9))} / 期望 {ntab}），"
                  "不覆盖缓存", file=sys.stderr)
            return 1

    stamp = head_stamp(cities_fc[0]["current"].get("time"), cities_fc[0].get("utc_offset_seconds"))
    header = f"# weather {stamp} cities={len(CITIES)} campus={CAMPUS}"
    # 头行 + P + C/H/D 行，每行恰好一个 \n，末尾没有多余空行
    text = header + "\n" + "".join(r + "\n" for r in lines)

    try:
        atomic_write(out_path, text)
    except OSError as e:
        print(f"weather: 写文件失败（旧缓存未动）—— {type(e).__name__}: {e}", file=sys.stderr)
        return 1

    print(f"weather {n_c} cities / H {n_h} / D {n_d} -> {out_path} "
          f"({time.monotonic() - t0:.1f}s)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())