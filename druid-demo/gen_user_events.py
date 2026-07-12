#!/usr/bin/env python3
"""
生成 1000 万条电商用户行为日志 (JSON Lines)

数据时间范围: 2024-06-01 ~ 2024-06-07 (7 天)
输出: 10 个文件, 每个文件 100 万行
预计大小: ~2.5 GB
预计耗时: ~2 分钟

用法:
    python3 gen_user_events.py [--output /data/user_events] [--rows 10000000]

依赖: 仅标准库
"""

import argparse
import json
import random
import time
from datetime import datetime, timedelta
from pathlib import Path

# ============ 默认配置 ============
DEFAULT_TOTAL_ROWS = 10_000_000
DEFAULT_OUTPUT_DIR = "/data/user_events"
ROWS_PER_FILE = 1_000_000

START_TIME = datetime(2024, 6, 1, 0, 0, 0)
END_TIME = datetime(2024, 6, 8, 0, 0, 0)  # 7 天

# ============ 维度池 ============
DEVICES = ["mobile", "pc", "tablet"]
DEVICE_WEIGHTS = [0.65, 0.25, 0.10]
OS_MAP = {
    "mobile": ["iOS", "Android"],
    "pc": ["Windows", "macOS"],
    "tablet": ["iOS", "Android"],
}

CHANNELS = ["organic", "ad", "push", "email", "social"]

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

USER_POOL_SIZE = 500_000     # 50 万独立用户
PRODUCT_POOL_SIZE = 10_000   # 1 万商品


def gen_user_id():
    return f"U{random.randint(1, USER_POOL_SIZE):07d}"


def gen_product():
    pid = f"P{random.randint(1, PRODUCT_POOL_SIZE):05d}"
    cat = random.choices(CATS, weights=CAT_WEIGHTS, k=1)[0]
    return pid, cat


def gen_event():
    return random.choices(EVENT_TYPES, weights=EVENT_WEIGHTS, k=1)[0]


def gen_amount_quantity(event_type):
    """根据事件类型生成金额和数量"""
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


def gen_device_os():
    device = random.choices(DEVICES, weights=DEVICE_WEIGHTS, k=1)[0]
    os_name = random.choice(OS_MAP[device])
    return device, os_name


def gen_location():
    country = random.choices(COUNTRIES, weights=COUNTRY_WEIGHTS, k=1)[0]
    if country == "CN":
        province = random.choice(CN_PROVINCES)
    else:
        province = ""
    return country, province


def gen_timestamp():
    """在 7 天范围内均匀随机生成时间戳"""
    total_seconds = int((END_TIME - START_TIME).total_seconds())
    return (START_TIME + timedelta(seconds=random.randint(0, total_seconds - 1))).isoformat()


def generate(total_rows, output_dir):
    output_path = Path(output_dir)
    output_path.mkdir(parents=True, exist_ok=True)

    print(f"配置:")
    print(f"  总行数:   {total_rows:,}")
    print(f"  输出目录: {output_path}")
    print(f"  时间范围: {START_TIME.date()} ~ {END_TIME.date() - timedelta(days=1)} (7 天)")
    print(f"  每文件:   {ROWS_PER_FILE:,} 行")
    print()

    start = time.time()
    file_idx = 0

    for batch_start in range(0, total_rows, ROWS_PER_FILE):
        file_idx += 1
        filename = output_path / f"events_part_{file_idx:02d}.json"
        rows_this_file = min(ROWS_PER_FILE, total_rows - batch_start)

        with open(filename, "w", encoding="utf-8") as f:
            for _ in range(rows_this_file):
                event_type = gen_event()
                product_id, product_cat = gen_product()
                device, os_name = gen_device_os()
                country, province = gen_location()
                amount, quantity = gen_amount_quantity(event_type)

                record = {
                    "event_time": gen_timestamp(),
                    "user_id": gen_user_id(),
                    "device": device,
                    "os": os_name,
                    "channel": random.choice(CHANNELS),
                    "event_type": event_type,
                    "product_id": product_id,
                    "product_cat": product_cat,
                    "country": country,
                    "province": province,
                    "amount": amount,
                    "quantity": quantity,
                }
                f.write(json.dumps(record, ensure_ascii=False) + "\n")

        elapsed = time.time() - start
        done_rows = batch_start + rows_this_file
        speed = done_rows / elapsed if elapsed > 0 else 0
        print(f"  [{file_idx:2d}] {filename.name:<28s} | 累计 {done_rows:>10,} 行 | {elapsed:6.1f}s | {speed:,.0f} rows/s")

    elapsed = time.time() - start
    print()
    print(f"完成! 共 {total_rows:,} 行, {file_idx} 个文件, 耗时 {elapsed:.1f}s")

    # 输出样例
    sample_file = output_path / "events_part_01.json"
    if sample_file.exists():
        with open(sample_file) as f:
            print(f"\n样例数据:")
            for _ in range(3):
                print(f"  {f.readline().strip()}")

    print(f"\n下一步: 修改 user_events_ingest.json 中的 baseDir 为 '{output_path}'")
    print(f"        然后提交到 Druid:")

    print(f"        curl -X POST 'http://localhost:8888/druid/indexer/v1/task' \\")
    print(f"          -H 'Content-Type: application/json' \\")
    print(f"          -d @user_events_ingest.json")


def main():
    parser = argparse.ArgumentParser(description="生成电商用户行为日志 (JSON Lines)")
    parser.add_argument("-o", "--output", default=DEFAULT_OUTPUT_DIR,
                        help=f"输出目录 (默认: {DEFAULT_OUTPUT_DIR})")
    parser.add_argument("-n", "--rows", type=int, default=DEFAULT_TOTAL_ROWS,
                        help=f"总行数 (默认: {DEFAULT_TOTAL_ROWS})")
    args = parser.parse_args()

    if args.rows <= 0:
        parser.error("--rows 必须为正整数")

    generate(args.rows, args.output)


if __name__ == "__main__":
    main()
