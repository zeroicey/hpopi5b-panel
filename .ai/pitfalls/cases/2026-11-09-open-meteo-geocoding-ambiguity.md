# Open-Meteo geocoding 把中文小镇名解析到几百公里外（甚至另一个国家）

**Severity:** 🟠 medium（不报警、不报错，只是天气数据悄悄来自错误地点）· **First hit:** 2026-11-09 · **Hits since:** 0

## Symptom

给天气幕选城市时，用 Open-Meteo geocoding API 搜中文地名：

- `常平` ⇒ 返回 **河南焦作** / **朝鲜** 的同名结果，广东东莞的常平镇排在后面或根本不出现；
- `利川`（中文）⇒ 结果为空；
- `Lichuan`（英文）⇒ 能给出湖北恩施一带（约 30.3,108.85），但精度只到县。

若抓取器每次启动都动态 geocode，同一份代码在不同时间可能拿到不同城市 —— 而屏幕上只显示
「东莞 · 常平」四个字，用户看不出来数据其实来自河南。

## Root cause

Open-Meteo geocoding 是按名字匹配的通用地名库，`name` 相同/相似的结果多，排序受语言、人口、
feature code 影响；中国「镇/街道」级地名没有稳定唯一键。这不是 API 坏了，是**用名字当主键**本身不成立。

## Fix

1. **坐标写死**在 `tools/fetch-weather.py` 的 `CITIES` 表里，抓取时只传 `latitude/longitude`，
   运行时不再 geocode。
2. 用「常平镇」而不是「常平」做一次性人工核对（22.97692,113.99024）；利川用 Wikipedia/公开坐标
   （30.29649,108.93776），不依赖 geocoding 的中文结果。
3. 城市显示名与坐标一起写在表里；改名/换城市只改这张表，C 侧布局不随城市数变。

## Guard

```sh
grep -A8 '^CITIES' tools/fetch-weather.py          # 坐标是人核过的固定表，不是 geocode 输出
head -2 /var/cache/hpopi5b-panel/weather.tsv       # P 行主城 / 第一条 C 行
grep '^C\t2\t' /var/cache/hpopi5b-panel/weather.tsv  # 利川应在 7~15°C 量级（冬季），不是北方 -5°C
```

判据：`weather.tsv` 的 `C` 行温度与已知城市量级相符（利川冬季明显低于佛山/深圳；
常平与深圳/广州同量级）；若「常平」突然和河南/北方城市同温，先查坐标。

**通用教训**：外部数据源用「人类可读名字」做查询键时，必须把命中结果当成不可信输入；
能写死稳定 ID/坐标就别在运行时解析。