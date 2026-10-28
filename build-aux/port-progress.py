#!/usr/bin/env python3
"""Recompute the progress table of docs/PORT_STATUS.md from its rows.

Run it before committing a port; it rewrites the block between the
PROGRESS markers. `ported` and `dropped` rows count as done.
"""

import re
import sys
from pathlib import Path

PATH = Path(__file__).resolve().parent.parent / 'docs' / 'PORT_STATUS.md'
ROW = re.compile(r'^\| `(js/[^`]+)` \| (\d+) \| (\w+) \|')
DONE = {'ported', 'dropped'}


def main():
    text = PATH.read_text()
    totals = {}
    milestone = None
    for line in text.splitlines():
        if line.startswith('## '):
            milestone = line[3:].strip()
            continue
        m = ROW.match(line)
        if not m or milestone is None:
            continue
        lines, status = int(m.group(2)), m.group(3)
        t = totals.setdefault(milestone, [0, 0, 0, 0])
        t[0] += 1
        t[1] += lines
        if status in DONE:
            t[2] += 1
            t[3] += lines

    def row(name, t):
        return (f'| {name} | {t[2]}/{t[0]} | {100 * t[2] / t[0]:.0f}% '
                f'| {t[3]}/{t[1]} | {100 * t[3] / t[1]:.1f}% |')

    grand = [sum(t[i] for t in totals.values()) for i in range(4)]
    table = ['| Milestone | modules done | % | JS lines done | % |',
             '|---|---:|---:|---:|---:|']
    table += [row(k, t) for k, t in totals.items()]
    table.append(row('**total**', grand))
    block = '\n'.join(table)

    new, n = re.subn(r'(<!-- PROGRESS -->\n)(?:.*?\n)?(<!-- /PROGRESS -->)',
                     lambda m: m.group(1) + block + '\n' + m.group(2), text,
                     flags=re.S)
    if n != 1:
        sys.exit('docs/PORT_STATUS.md has no PROGRESS block')
    if new != text:
        PATH.write_text(new)
    print(block)


if __name__ == '__main__':
    main()
