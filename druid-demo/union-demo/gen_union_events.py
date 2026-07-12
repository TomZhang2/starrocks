#!/usr/bin/env python3
"""
生成两个电商用户行为表 (App 端 + Web 端), 用于验证 UNION / JOIN 等场景。

表设计:
  - app_events: 600 万行, App 端行为, 独有字段 device/os/channel/app_version
  - web_events: 400 万行, Web 端行为, 独有字段 browser/traffic_source/page_url/session_id

公共字段 (UNION ALL 可用):
  event_time, user_id, event_type, product_id, product_cat, country, province, amount, quantity

用户池设计 (验证 JOIN):
  - App 独占用户: 35 万
  - Web 独占用户: 10 万
  - 跨端用户:     5 万  (两个表都有, 可 JOIN)

用法:
    python3 gen_union_events.py [--output /data/union-demo] [--app-rows 6000000] [--web-rows 4000000]

依赖: 仅标准库
"""

import argparse
import json
import random
import time
from datetime import datetime, timedelta
from pathlib import Path

# ============ 默认配置 ============
DEFAULT_APP_ROWS = 6_000_000
DEFAULT_WEB_ROWS = 4_000_000
DEFAULT_OUTPUT_DIR = "/data/union-demo"
ROWS_PER_FILE = 1_000_000

START_TIME = datetime(2024, 6, 1, 0, 0, 0)
END_TIME = datetime(2024, 6, 8, 0, 0, 0)  # 7 天

# ============ 用户池划分 ============
# 总 50 万用户, 分三组
APP_ONLY_START = 1          # U0000001 ~ U0350000: App 独占 (35 万)
APP_ONLY_END   = 350_000
WEB_ONLY_START = 350_001    # U0350001 ~ U0450000: Web 独占 (10 万)
WEB_ONLY_END   = 450_000
CROSS_START    = 450_001    # U0450001 ~ U0500000: 跨端用户 (5 万)
CROSS_END      = 500_000

# App 用户池 = App 独占 + 跨端
APP_USER_POOL = list(range(APP_ONLY_START, APP_ONLY_END + 1)) + \
                list(range(CROSS_START, CROSS_END + 1))
# Web 用户池 = Web 独占 + 跨端
WEB_USER_POOL = list(range(WEB_ONLY_START, WEB_ONLY_END + 1)) + \
                list(range(CROSS_START, CROSS_END + 1))

# ============ 公共维度池 ============
EVENT_TYPES = ["view", "click", "cart", "order", "pay"]
EVENT_WEIGHTS = [0.55, 0.25, 0.10, 0.07, 0.03]

CATS = ["electronics", "clothing", "food", "books", "home"]
CAT_WEIGHTS = [0.25, 0.30, 0.20, 0.10, 0.15]

COUNTRIES = ["CN", "US", "JP", "KR", "DE", "UK", "FR"]
COUNTRY_WEIGHTS = [0.60, 0.15, 0.08, 0.05, 0.04, 0.04, 0.04]

CN_PROVINCES = [
    "beijing", "shanghai", "guangdong", "zhejiang", "jiangsu",
    "sichuan", "hubei", "fujian", "shandong", "others",
]

PRODUCT_POOL_SIZE = 10_000

# ============ App 独有维度池 ============
APP_DEVICES = ["mobile", "tablet"]
APP_DEVICE_WEIGHTS = [0.85, 0.15]
APP_OS = {"mobile": ["iOS", "Android"], "tablet": ["iOS", "Android"]}
APP_CHANNELS = ["organic", "ad", "push", "email", "social"]
APP_VERSIONS = ["1.0.0", "1.5.0", "2.0.0", "2.3.0", "2.5.0", "3.0.0", "3.2.0", "3.5.0"]
APP_VERSION_WEIGHTS = [0.05, 0.05, 0.08, 0.10, 0.15, 0.20, 0.22, 0.15]

# ============ Web 独有维度池 ============
WEB_BROWSERS = ["chrome", "safari", "firefox", "edge"]
WEB_BROWSER_WEIGHTS = [0.55, 0.25, 0.12, 0.08]
WEB_TRAFFIC_SOURCES = ["organic", "ad", "social", "direct", "referral"]
WEB_PAGE_TEMPLATES = ["/product/{pid}", "/home", "/search", "/cart", "/checkout", "/category/{cat}"]
WEB_PAGE_WEIGHTS = [0.50, 0.15, 0.15, 0.08, 0.05, 0.07]


# ============ 公共字段生成函数 ============

def gen_user_id(pool):
    return f"U{random.choice(pool):07d}"


def gen_product():
    pid = f"P{random.randint(1, PRODUCT_POOL_SIZE):05d}"
    cat = random.choices(CATS, weights=CAT_WEIGHTS, k=1)[0]
    return pid, cat


def gen_event():
    return random.choices(EVENT_TYPES, weights=EVENT_WEIGHTS, k=1)[0]


def gen_amount_quantity(event_type):
    if event_type in ("view", "click"):
        return 0.0, 0
    quantity = random.choices([1, 1, 1, 2, 2, 3, 5], k=1)[0]
    if event_type == "cart":
        amount = round(random.uniform(10, 2000) * quantity, 2)
    elif event_type == "order":
        amount = round(random.uniform(50, 5000) * quantity, 2)
    else:  # pay
        amount = round(random.uniform(50, 5000) * quantity * 0.9, 2)
    return amount, quantity


def gen_location():
    country = random.choices(COUNTRIES, weights=COUNTRY_WEIGHTS, k=1)[0]
    province = random.choice(CN_PROVINCES) if country == "CN" else ""
    return country, province


def gen_timestamp():
    total_seconds = int((END_TIME - START_TIME).total_seconds())
    return (START_TIME + timedelta(seconds=random.randint(0, total_seconds - 1))).isoformat()


# ============ App 独有字段生成 ============

def gen_app_fields():
    device = random.choices(APP_DEVICES, weights=APP_DEVICE_WEIGHTS, k=1)[0]
    os_name = random.choice(APP_OS[device])
    channel = random.choice(APP_CHANNELS)
    app_version = random.choices(APP_VERSIONS, weights=APP_VERSION_WEIGHTS, k=1)[0]
    return {"device": device, "os": os_name, "channel": channel, "app_version": app_version}


def gen_app_record():
    event_type = gen_event()
    product_id, product_cat = gen_product()
    country, province = gen_location()
    amount, quantity = gen_amount_quantity(event_type)
    record = {
        "event_time": gen_timestamp(),
        "user_id": gen_user_id(APP_USER_POOL),
        "event_type": event_type,
        "product_id": product_id,
        "product_cat": product_cat,
        "country": country,
        "province": province,
        "amount": amount,
        "quantity": quantity,
    }
    record.update(gen_app_fields())
    return record


# ============ Web 独有字段生成 ============

def gen_web_fields(product_id, product_cat):
    browser = random.choices(WEB_BROWSERS, weights=WEB_BROWSER_WEIGHTS, k=1)[0]
    traffic_source = random.choice(WEB_TRAFFIC_SOURCES)
    template = random.choices(WEB_PAGE_TEMPLATES, weights=WEB_PAGE_WEIGHTS, k=1)[0]
    page_url = template.format(pid=product_id, cat=product_cat)
    session_id = f"S{random.randint(1, 2_000_000):08d}"
    return {
        "browser": browser,
        "traffic_source": traffic_source,
        "page_url": page_url,
        "session_id": session_id,
    }


def gen_web_record():
    event_type = gen_event()
    product_id, product_cat = gen_product()
    country, province = gen_location()
    amount, quantity = gen_amount_quantity(event_type)
    record = {
        "event_time": gen_timestamp(),
        "user_id": gen_user_id(WEB_USER_POOL),
        "event_type": event_type,
        "product_id": product_id,
        "product_cat": product_cat,
        "country": country,
        "province": province,
        "amount": amount,
        "quantity": quantity,
    }
    record.update(gen_web_fields(product_id, product_cat))
    return record


# ============ 批量生成 ============

def generate_table(gen_func, total_rows, output_dir, prefix):
    """生成一个表的数据, 分多个文件"""
    output_path = Path(output_dir)
    output_path.mkdir(parents=True, exist_ok=True)

    print(f"  生成 {prefix} 表: {total_rows:,} 行 -> {output_path}")
    start = time.time()
    file_idx = 0

    for batch_start in range(0, total_rows, ROWS_PER_FILE):
        file_idx += 1
        filename = output_path / f"{prefix}_part_{file_idx:02d}.json"
        rows_this_file = min(ROWS_PER_FILE, total_rows - batch_start)

        with open(filename, "w", encoding="utf-8") as f:
            for _ in range(rows_this_file):
                f.write(json.dumps(gen_func(), ensure_ascii=False) + "\n")

        elapsed = time.time() - start
        done = batch_start + rows_this_file
        speed = done / elapsed if elapsed > 0 else 0
        print(f"    [{file_idx:2d}] {filename.name:<28s} | 累计 {done:>10,} | {elapsed:6.1f}s | {speed:,.0f} rows/s")

    elapsed = time.time() - start
    print(f"    完成: {total_rows:,} 行, {file_idx} 文件, {elapsed:.1f}s")
    return file_idx


def main():
    parser = argparse.ArgumentParser(description="生成 App + Web 双表数据 (用于验证 UNION/JOIN)")
    parser.add_argument("-o", "--output", default=DEFAULT_OUTPUT_DIR,
                        help=f"输出根目录 (默认: {DEFAULT_OUTPUT_DIR})")
    parser.add_argument("--app-rows", type=int, default=DEFAULT_APP_ROWS,
                        help=f"App 表行数 (默认: {DEFAULT_APP_ROWS})")
    parser.add_argument("--web-rows", type=int, default=DEFAULT_WEB_ROWS,
                        help=f"Web 表行数 (默认: {DEFAULT_WEB_ROWS})")
    args = parser.parse_args()

    if args.app_rows <= 0 or args.web_rows <= 0:
        parser.error("--app-rows 和 --web-rows 必须为正整数")

    print("=" * 70)
    print("App + Web 双表数据生成 (用于验证 UNION / JOIN)")
    print("=" * 70)
    print(f"配置:")
    print(f"  输出目录:   {args.output}")
    print(f"  App 表行数: {args.app_rows:,}  -> {args.output}/app/")
    print(f"  Web 表行数: {args.web_rows:,}  -> {args.output}/web/")
    print(f"  时间范围:   {START_TIME.date()} ~ {END_TIME.date() - timedelta(days=1)} (7 天)")
    print()
    print(f"用户池设计 (总 50 万用户):")
    print(f"  App 独占: U0000001~U0350000 (35 万)")
    print(f"  Web 独占: U0350001~U0450000 (10 万)")
    print(f"  跨端用户: U0450001~U0500000 (5 万)  <- 两个表都有, 可 JOIN")
    print(f"  App 用户池: {len(APP_USER_POOL):,} (App独占 + 跨端)")
    print(f"  Web 用户池: {len(WEB_USER_POOL):,} (Web独占 + 跨端)")
    print()
    print("字段设计:")
    print(f"  公共字段: event_time, user_id, event_type, product_id, product_cat,")
    print(f"            country, province, amount, quantity")
    print(f"  App 独有: device, os, channel, app_version")
    print(f"  Web 独有: browser, traffic_source, page_url, session_id")
    print()

    total_start = time.time()

    print(">>> 阶段 1/2: 生成 App 表数据")
    generate_table(gen_app_record, args.app_rows, f"{args.output}/app", "app")
    print()

    print(">>> 阶段 2/2: 生成 Web 表数据")
    generate_table(gen_web_record, args.web_rows, f"{args.output}/web", "web")
    print()

    total_elapsed = time.time() - total_start
    total_rows = args.app_rows + args.web_rows
    print("=" * 70)
    print(f"全部完成! 共 {total_rows:,} 行, 耗时 {total_elapsed:.1f}s")
    print(f"  App: {args.output}/app/ ({args.app_rows:,} 行)")
    print(f"  Web: {args.output}/web/ ({args.web_rows:,} 行)")
    print("=" * 70)

    # 输出样例
    print("\nApp 表样例:")
    app_sample = Path(f"{args.output}/app/app_part_01.json")
    if app_sample.exists():
        with open(app_sample) as f:
            for i, line in enumerate(f):
                if i >= 2:
                    break
                print(f"  {line.strip()}")

    print("\nWeb 表样例:")
    web_sample = Path(f"{args.output}/web/web_part_01.json")
    if web_sample.exists():
        with open(web_sample) as f:
            for i, line in enumerate(f):
                if i >= 2:
                    break
                print(f"  {line.strip()}")

    print(f"\n下一步:")
    print(f"  1. 提交 App 表导入:")
    print(f"     curl -X POST 'http://localhost:8888/druid/indexer/v1/task' \\")
    print(f"       -H 'Content-Type: application/json' \\")
    print(f"       -d @app_events_ingest.json")
    print(f"  2. 提交 Web 表导入:")
    print(f"     curl -X POST 'http://localhost:8888/druid/indexer/v1/task' \\")
    print(f"       -H 'Content-Type: application/json' \\")
    print(f"       -d @web_events_ingest.json")
    print(f"  3. 执行 UNION/JOIN 查询 (见 README.md)")


if __name__ == "__main__":
    main()
