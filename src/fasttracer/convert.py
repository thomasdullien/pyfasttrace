"""Convert .ftrc binary trace files to Chrome Trace JSON.

Supports format v2 (12-byte events, uint32 func_id, exit flag in flags byte).
"""

import argparse
import json
import struct
import sys
from pathlib import Path

# Must match fasttracer.h v2
MAGIC = 0x43525446  # "FTRC" little-endian
VERSION = 2
FLAG_EXIT = 0x80       # bit 7 of flags byte
FLAG_C_FUNCTION = 0x01

# struct BufferHeader layout v2:
#   uint32 magic, version, pid, num_strings
#   int64  base_ts_ns
#   uint32 num_events
#   uint8  num_threads, _pad1
#   uint16 _pad2
#   uint64 thread_table[256]
#   uint32 string_table_offset, events_offset
HEADER_FMT = "<IIIIqIBBH"
HEADER_SIZE_BEFORE_THREADS = struct.calcsize(HEADER_FMT)
THREAD_TABLE_SIZE = 256 * 8  # 256 × uint64
HEADER_TAIL_FMT = "<II"  # string_table_offset, events_offset
FULL_HEADER_SIZE = HEADER_SIZE_BEFORE_THREADS + THREAD_TABLE_SIZE + struct.calcsize(HEADER_TAIL_FMT)

# 12-byte event: ts_delta_us(u32), func_id(u32), tid_idx(u8), flags(u8), _pad(u16)
EVENT_FMT = "<IIBBH"
EVENT_SIZE = struct.calcsize(EVENT_FMT)


def read_string_table(data, offset, num_strings):
    """Read string table entries. Returns list of strings indexed by func_id (1-based)."""
    strings = ["<unknown>"]  # index 0 = placeholder
    pos = offset
    for _ in range(num_strings):
        if pos + 4 > len(data):
            break
        slen = struct.unpack_from("<I", data, pos)[0]
        pos += 4
        if pos + slen > len(data):
            break
        name = data[pos:pos + slen].decode("utf-8", errors="replace")
        strings.append(name)
        pos += slen
    return strings


def read_thread_table(data, offset, num_threads):
    """Read thread table. Returns list of OS thread IDs indexed by tid_idx."""
    threads = []
    for i in range(num_threads):
        os_tid = struct.unpack_from("<Q", data, offset + i * 8)[0]
        threads.append(os_tid)
    return threads


def convert_chunk(data, offset=0):
    """Parse one buffer chunk from data at offset. Returns (events, next_offset)."""
    if offset + FULL_HEADER_SIZE > len(data):
        return [], len(data)

    # Parse header
    fields = struct.unpack_from(HEADER_FMT, data, offset)
    magic, version, pid, num_strings, base_ts_ns, num_events, num_threads, _pad1, _pad2 = fields

    if magic != MAGIC:
        print(f"Warning: bad magic at offset {offset}: 0x{magic:08x}", file=sys.stderr)
        return [], len(data)

    if version != VERSION:
        print(f"Warning: unknown version {version} at offset {offset} (expected {VERSION})",
              file=sys.stderr)
        return [], len(data)

    # Thread table
    thread_table_offset = offset + HEADER_SIZE_BEFORE_THREADS
    threads = read_thread_table(data, thread_table_offset, num_threads)

    # String/events offsets
    tail_offset = thread_table_offset + THREAD_TABLE_SIZE
    st_offset, ev_offset = struct.unpack_from(HEADER_TAIL_FMT, data, tail_offset)

    # Read string table
    strings = read_string_table(data, offset + st_offset, num_strings)

    # Read events, converting B/E pairs into viztracer-compatible "X" events
    events = []
    thread_stacks = {}  # tid -> [(name, ts, flags), ...]
    ev_abs_offset = offset + ev_offset
    for i in range(num_events):
        eoff = ev_abs_offset + i * EVENT_SIZE
        if eoff + EVENT_SIZE > len(data):
            break
        ts_delta_us, func_id, tid_idx, flags, _pad = struct.unpack_from(EVENT_FMT, data, eoff)

        is_exit = bool(flags & FLAG_EXIT)

        # Reconstruct absolute timestamp in microseconds
        abs_ts_us = base_ts_ns / 1000.0 + ts_delta_us

        # Resolve function name
        if func_id < len(strings):
            name = strings[func_id]
        else:
            name = f"<func_{func_id}>"

        # Map thread ID
        os_tid = threads[tid_idx] if tid_idx < len(threads) else tid_idx

        if not is_exit:
            # Push entry onto per-thread stack
            stack = thread_stacks.setdefault(os_tid, [])
            stack.append((name, abs_ts_us, flags))
        else:
            # Pop matching entry, emit "X" event
            stack = thread_stacks.get(os_tid)
            if stack:
                entry_name, entry_ts, entry_flags = stack.pop()
                event = {
                    "ph": "X",
                    "name": entry_name,
                    "ts": entry_ts,
                    "dur": abs_ts_us - entry_ts,
                    "pid": pid,
                    "tid": os_tid,
                    "cat": "FEE",
                }
                events.append(event)

    # Next chunk starts after our events
    next_offset = ev_abs_offset + num_events * EVENT_SIZE
    return events, next_offset


def convert_file(input_path):
    """Read an .ftrc file and return all Chrome Trace events.

    Uses the native C library (libftrc) for parsing. Falls back to
    the pure-Python implementation if the C extension is unavailable.
    """
    try:
        from fasttracer._fasttracer import read_ftrc
        return read_ftrc(str(input_path))
    except ImportError:
        pass

    # Pure-Python fallback
    data = Path(input_path).read_bytes()
    all_events = []
    offset = 0
    while offset < len(data):
        events, next_offset = convert_chunk(data, offset)
        all_events.extend(events)
        if next_offset <= offset:
            break  # no progress, avoid infinite loop
        offset = next_offset
    return all_events


def main():
    parser = argparse.ArgumentParser(description="Convert .ftrc binary traces to Chrome Trace JSON")
    parser.add_argument("input", nargs="+", help="Input .ftrc file(s)")
    parser.add_argument("-o", "--output", default="trace.json", help="Output JSON file")
    args = parser.parse_args()

    all_events = []
    for path in args.input:
        events = convert_file(path)
        all_events.extend(events)
        print(f"Read {len(events)} events from {path}", file=sys.stderr)

    # Sort by timestamp
    all_events.sort(key=lambda e: e["ts"])

    trace = {
        "traceEvents": all_events,
        "viztracer_metadata": {"version": "0.0.1", "overflow": False},
        "file_info": {"files": {}, "functions": {}},
    }

    with open(args.output, "w") as f:
        json.dump(trace, f)

    print(f"Wrote {len(all_events)} events to {args.output}", file=sys.stderr)


if __name__ == "__main__":
    main()
