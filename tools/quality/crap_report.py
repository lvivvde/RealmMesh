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
    with open(path, newline="") as f:
        for row in csv.reader(f):
            # NLOC,CCN,token,PARAM,length,location,file,function,long_name,start,end
            if len(row) < 11 or not row[1].isdigit():
                continue
            functions[os.path.realpath(row[6])].append(
                {"ccn": int(row[1]), "name": row[7], "start": int(row[9]),
                 "end": int(row[10]), "regions": {}})
    return functions


def innermost(candidates, line):
    best = None
    for fn in candidates:
        if fn["start"] <= line <= fn["end"]:
            if best is None or fn["end"] - fn["start"] < best["end"] - best["start"]:
                best = fn
    return best


def merge_coverage(path, functions):
    with open(path) as f:
        data = json.load(f)["data"][0]
    covered_files = {os.path.realpath(entry["filename"]) for entry in data["files"]}
    for record in data["functions"]:
        main_file = os.path.realpath(record["filenames"][0])
        candidates = functions.get(main_file)
        if not candidates:
            continue
        # region: [lineStart, colStart, lineEnd, colEnd, count, fileID, expandedFileID, kind]
        own = [r for r in record["regions"] if r[5] == 0 and r[7] == 0]
        if not own:
            continue
        fn = innermost(candidates, min(r[0] for r in own))
        if fn is None:
            continue
        for r in own:
            key = (r[0], r[1], r[2], r[3])
            fn["regions"][key] = max(fn["regions"].get(key, 0), r[4])
    return covered_files


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--lizard-csv", required=True)
    parser.add_argument("--llvm-cov-json", required=True)
    parser.add_argument("--top", type=int, default=30)
    parser.add_argument("--threshold", type=float, default=30.0)
    args = parser.parse_args()

    functions = load_lizard(args.lizard_csv)
    covered_files = merge_coverage(args.llvm_cov_json, functions)
    root = os.getcwd() + os.sep

    rows = []
    skipped = 0
    for path, fns in functions.items():
        if path not in covered_files:
            skipped += len(fns)
            continue
        for fn in fns:
            regions = fn["regions"]
            cov = (sum(1 for c in regions.values() if c > 0) / len(regions)) if regions else 0.0
            crap = fn["ccn"] ** 2 * (1 - cov) ** 3 + fn["ccn"]
            rows.append((crap, fn["ccn"], cov, path.removeprefix(root), fn["start"], fn["name"]))

    rows.sort(reverse=True)
    over = sum(1 for r in rows if r[0] > args.threshold)
    print(f"{'CRAP':>8} {'CCN':>4} {'COV':>5}  LOCATION  FUNCTION")
    for crap, ccn, cov, path, line, name in rows[:args.top]:
        print(f"{crap:8.1f} {ccn:4d} {cov:5.0%}  {path}:{line}  {name}")
    print(f"\n{len(rows)} functions scored, {over} above CRAP {args.threshold:g}; "
          f"{skipped} functions in files without coverage data skipped.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
