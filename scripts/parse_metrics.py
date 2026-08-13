#!/usr/bin/env python3
"""Parse auroraOS QEMU metrics output into a Markdown report.

Reads raw serial output captured by benchmark_runner.py (or any QEMU log
containing `cat /proc/irq`, `cat /proc/power`, `free` command results) and
produces a Markdown table of real measured values.

The on-target metrics are produced by the Metrics subsystem (kernel/metrics)
and exposed through ProcFS nodes mounted in apps/kernel.cpp:
  - /proc/irq     -> irq_latency / ctx_switch / heap_alloc (avg/max/p99/count)
  - /proc/latency -> same + p99 for all three recorders
  - /proc/power   -> sleep_ratio / sleep_count / dirty_ratio
  - /proc/meminfo -> MemTotal / MemFree / Defrag
  - /proc/net     -> udp_drops
  - /proc/softbus -> registers
"""
import re
import argparse


def parse_kv_pair(block, key):
    """Extract 'key avg=1us max=2us p99=3us count=4' style fields."""
    m = re.search(key + r"[^\n]*", block)
    if not m:
        return None
    line = m.group(0)
    vals = {}
    for field in ("avg", "max", "p99", "count"):
        fm = re.search(field + r"=(\d+)", line)
        if fm:
            vals[field] = int(fm.group(1))
    return vals if vals else None


def parse_metrics(content):
    """Return a dict of metric -> (value, unit) extracted from raw output."""
    metrics = {}

    # irq_latency / ctx_switch / heap_alloc come from `/proc/irq` or `/proc/latency`
    for key in ("irq_latency", "ctx_switch", "heap_alloc", "heap_64b"):
        vals = parse_kv_pair(content, key)
        if vals:
            metrics[key] = vals

    # sleep ratio / dirty ratio / sleep count from `/proc/power`
    m = re.search(r"sleep_ratio (\d+)% sleep_count (\d+)", content)
    if m:
        metrics["sleep_ratio"] = int(m.group(1))
        metrics["sleep_count"] = int(m.group(2))
    m = re.search(r"dirty_ratio (\d+)%", content)
    if m:
        metrics["dirty_ratio"] = int(m.group(1))

    # memory from `free` (/proc/meminfo)
    m = re.search(r"MemTotal:\s+(\d+)", content)
    if m:
        metrics["mem_total"] = int(m.group(1))
    m = re.search(r"MemFree:\s+(\d+)", content)
    if m:
        metrics["mem_free"] = int(m.group(1))
    m = re.search(r"Defrag:\s+(\d+)", content)
    if m:
        metrics["defrag"] = int(m.group(1))

    # udp drops / softbus registers
    m = re.search(r"udp_drops (\d+)", content)
    if m:
        metrics["udp_drops"] = int(m.group(1))
    m = re.search(r"registers (\d+)", content)
    if m:
        metrics["softbus_registers"] = int(m.group(1))

    return metrics


def fmt_latency(vals):
    """Format a {avg,max,p99,count} dict as a compact string."""
    if not vals:
        return "N/A"
    s = "%d us (avg)" % vals.get("avg", 0)
    if "max" in vals:
        s += ", %d us (max)" % vals["max"]
    if "p99" in vals:
        s += ", %d us (p99)" % vals["p99"]
    if "count" in vals:
        s += ", n=%d" % vals["count"]
    return s


def generate_report(metrics):
    lines = []
    lines.append("### Automated Metrics Report")
    lines.append("")
    lines.append("| Metric | Value |")
    lines.append("|---|---|")

    if "irq_latency" in metrics and metrics["irq_latency"].get("count", 0) > 0:
        lines.append("| IRQ Latency | %s |" % fmt_latency(metrics["irq_latency"]))
    else:
        lines.append("| IRQ Latency | not sampled (no ISR sampling hook) |")

    if "ctx_switch" in metrics:
        lines.append("| Context Switch | %s |" % fmt_latency(metrics["ctx_switch"]))

    heap_key = "heap_alloc" if "heap_alloc" in metrics else "heap_64b"
    if heap_key in metrics:
        lines.append("| Heap Allocation (64B) | %s |" % fmt_latency(metrics[heap_key]))

    if "sleep_ratio" in metrics:
        lines.append("| Sleep Ratio | %d %% |" % metrics["sleep_ratio"])
    if "dirty_ratio" in metrics:
        lines.append("| Dirty Ratio | %d %% |" % metrics["dirty_ratio"])
    if "mem_total" in metrics and "mem_free" in metrics:
        lines.append("| Heap Memory | %d B total / %d B free |" % (
            metrics["mem_total"], metrics["mem_free"]))
    if "defrag" in metrics:
        lines.append("| Heap Defrags | %d times |" % metrics["defrag"])
    if "udp_drops" in metrics:
        lines.append("| UDP Drops | %d |" % metrics["udp_drops"])
    if "softbus_registers" in metrics:
        lines.append("| SoftBus Registers | %d |" % metrics["softbus_registers"])

    # Honest note: QEMU lm3s6965evb does not emulate the Cortex-M3 DWT cycle
    # counter, so latency averages read 0 while sample counts stay real.
    sampled = [k for k in ("ctx_switch", "heap_alloc", "heap_64b")
               if k in metrics and metrics[k].get("count", 0) > 0]
    if sampled and all(metrics[k].get("avg", 0) == 0 for k in sampled):
        lines.append("")
        lines.append("> Note: latency averages are 0 us because QEMU does not "
                     "emulate the DWT cycle counter. Sample counts are real.")
        lines.append("> Run on physical hardware (e.g. Apollo3 MiBand 8) for "
                     "non-zero cycle-accurate latency.")

    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("log", help="Raw QEMU serial output log file")
    parser.add_argument("--md", help="Markdown output file", default=None)
    args = parser.parse_args()

    with open(args.log, "r", encoding="utf-8", errors="replace") as f:
        content = f.read()

    metrics = parse_metrics(content)
    report = generate_report(metrics)

    if args.md:
        with open(args.md, "w", encoding="utf-8") as f:
            f.write(report)
        print("Metrics parsed and written to %s" % args.md)
    else:
        print(report)


if __name__ == "__main__":
    main()
