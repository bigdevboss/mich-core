#!/usr/bin/env python3
"""Parse `Mich netbench:` serial lines into a flat CSV.

The guest reports raw TSC cycle counts because cycles are the only figure that
survives being moved between a TCG sandbox and real KVM hardware unchanged; wall
time and throughput only become comparable once a real invariant-TSC frequency is
known. Pass that frequency with --tsc-hz (from `lscpu`, the "CPU max MHz" or the
model's rated base clock on invariant-TSC parts) to get derived microseconds and
throughput columns; without it those columns stay blank rather than guess.
"""

import argparse
import csv
import re
import sys

# Each guest line is `Mich netbench: <metric> key=value ...`; the metrics carry
# different key sets, so match them individually instead of one loose splitter.
LOOPBACK = re.compile(
    r"loopback-udp size=(\d+)B min=(\d+) avg=(\d+) p50=(\d+) p99=(\d+)")
WIRE_TX = re.compile(r"wire-tx bytes=(\d+) cycles=(\d+)")
WIRE_RR = re.compile(
    r"wire-rr size=(\d+)B min=(\d+) avg=(\d+) p50=(\d+) p99=(\d+)")
WIRE_UDP = re.compile(
    r"wire-udp-tx size=(\d+)B sent=(\d+) packets=(\d+) bytes=(\d+) cycles=(\d+)")


def us(cycles, hz):
    if not hz:
        return ""
    return round(cycles / hz * 1e6, 3)


def parse(lines, run, hz, rows):
    for line in lines:
        m = LOOPBACK.search(line)
        if m:
            size, mn, avg, p50, p99 = (int(x) for x in m.groups())
            rows.append({
                "run": run, "metric": "loopback-udp-rtt", "size_b": size,
                "min_cyc": mn, "avg_cyc": avg, "p50_cyc": p50, "p99_cyc": p99,
                "p50_us": us(p50, hz),
            })
            continue
        m = WIRE_TX.search(line)
        if m:
            byts, cyc = (int(x) for x in m.groups())
            # Bulk TX reports one aggregate cycle count for the whole transfer,
            # so throughput is bytes over that span rather than a percentile.
            mbps = round(byts / (cyc / hz) * 8 / 1e6, 3) if hz else ""
            rows.append({
                "run": run, "metric": "wire-tx-bulk", "size_b": byts,
                "p50_cyc": cyc, "p50_us": us(cyc, hz), "mbit_s": mbps,
            })
            continue
        m = WIRE_RR.search(line)
        if m:
            size, mn, avg, p50, p99 = (int(x) for x in m.groups())
            rows.append({
                "run": run, "metric": "wire-rr-rtt", "size_b": size,
                "min_cyc": mn, "avg_cyc": avg, "p50_cyc": p50, "p99_cyc": p99,
                "p50_us": us(p50, hz),
            })
            continue
        m = WIRE_UDP.search(line)
        if m:
            size, sent, pkts, byts, cyc = (int(x) for x in m.groups())
            rows.append({
                "run": run, "metric": "wire-udp-tx", "size_b": size,
                "packets": pkts, "p50_cyc": cyc, "p50_us": us(cyc, hz),
            })


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("logs", nargs="*", help="serial logs; stdin if omitted")
    ap.add_argument("--tsc-hz", type=float, default=0.0,
                    help="invariant TSC frequency in Hz for derived columns")
    ap.add_argument("-o", "--out", default="-", help="output CSV path or -")
    args = ap.parse_args()

    rows = []
    if args.logs:
        for i, path in enumerate(args.logs, 1):
            with open(path, encoding="ascii", errors="replace") as handle:
                parse(handle, i, args.tsc_hz, rows)
    else:
        parse(sys.stdin, 1, args.tsc_hz, rows)

    fields = ["run", "metric", "size_b", "packets", "min_cyc", "avg_cyc",
              "p50_cyc", "p99_cyc", "p50_us", "mbit_s"]
    out = sys.stdout if args.out == "-" else open(args.out, "w", newline="")
    writer = csv.DictWriter(out, fieldnames=fields)
    writer.writeheader()
    for row in rows:
        writer.writerow(row)
    if out is not sys.stdout:
        out.close()


if __name__ == "__main__":
    main()
