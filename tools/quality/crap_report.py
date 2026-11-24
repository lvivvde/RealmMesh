#!/usr/bin/env python3
"""按函数合并 lizard 圈复杂度与 llvm-cov 覆盖率，输出 CRAP 排行。

CRAP(m) = CCN(m)^2 * (1 - cov(m))^3 + CCN(m)，cov 取函数的 region 覆盖率。
同一函数的多个模板实例按 region 位置取最大执行次数；lambda 归入包住它的最内层
lizard 函数。覆盖数据里没有出现的文件整体跳过（不在 unit 范围内），出现了但
函数本身没有数据的按 0 覆盖计（内联/模板未被测试实例化）。

用法：crap_report.py --lizard-csv F --llvm-cov-json F [--top N] [--threshold T]
"""

import argparse
import csv
import json
import os
import sys
from collections import defaultdict


def load_lizard(path):
    functions = defaultdict(list)
    with open(path, newline="", encoding="utf-8") as source:
        for row in csv.reader(source):
            # NLOC,CCN,token,PARAM,length,location,file,function,long_name,start,end
            if len(row) < 11 or not row[1].isdigit():
                continue
            functions[os.path.realpath(row[6])].append(
                {
                    "ccn": int(row[1]),
                    "name": row[7],
                    "start": int(row[9]),
                    "end": int(row[10]),
                    "regions": {},
                }
            )
    return functions


def innermost(candidates, line):
    best = None
    for function in candidates:
        if function["start"] <= line <= function["end"]:
            if best is None or function["end"] - function["start"] < best["end"] - best["start"]:
                best = function
    return best


def merge_coverage(path, functions):
    with open(path, encoding="utf-8") as source:
        data = json.load(source)["data"][0]
    covered_files = {os.path.realpath(entry["filename"]) for entry in data["files"]}
    for record in data["functions"]:
        main_file = os.path.realpath(record["filenames"][0])
        candidates = functions.get(main_file)
        if not candidates:
            continue
        # region: [lineStart, colStart, lineEnd, colEnd, count, fileID, expandedFileID, kind]
        own = [region for region in record["regions"] if region[5] == 0 and region[7] == 0]
        if not own:
            continue
        function = innermost(candidates, min(region[0] for region in own))
        if function is None:
            continue
        for region in own:
            key = (region[0], region[1], region[2], region[3])
            function["regions"][key] = max(function["regions"].get(key, 0), region[4])
    return covered_files


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--lizard-csv", required=True)
    parser.add_argument("--llvm-cov-json", required=True)
    parser.add_argument("--top", type=int, default=30)
    parser.add_argument("--threshold", type=float, default=30.0)
    args = parser.parse_args()
    if args.top < 1:
        parser.error("--top must be a positive integer")

    functions = load_lizard(args.lizard_csv)
    covered_files = merge_coverage(args.llvm_cov_json, functions)
    root = os.getcwd() + os.sep

    rows = []
    skipped = 0
    for path, candidates in functions.items():
        if path not in covered_files:
            skipped += len(candidates)
            continue
        for function in candidates:
            regions = function["regions"]
            coverage = (
                sum(1 for count in regions.values() if count > 0) / len(regions)
                if regions
                else 0.0
            )
            crap = function["ccn"] ** 2 * (1 - coverage) ** 3 + function["ccn"]
            rows.append(
                (
                    crap,
                    function["ccn"],
                    coverage,
                    path.removeprefix(root),
                    function["start"],
                    function["name"],
                )
            )

    rows.sort(reverse=True)
    over = sum(1 for row in rows if row[0] > args.threshold)
    print(f"{'CRAP':>8} {'CCN':>4} {'COV':>5}  LOCATION  FUNCTION")
    for crap, ccn, coverage, path, line, name in rows[: args.top]:
        print(f"{crap:8.1f} {ccn:4d} {coverage:5.0%}  {path}:{line}  {name}")
    print(
        f"\n{len(rows)} functions scored, {over} above CRAP {args.threshold:g}; "
        f"{skipped} functions in files without coverage data skipped."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
