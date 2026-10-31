#!/usr/bin/env python3
# Finds calls that pass g_steal_pointer (&x) and also read x in another
# argument (or in the function they call): C does not say in which order the
# arguments are evaluated, and GCC often steals first, so the other argument
# reads NULL. That crashed the shell at login with an MTP device plugged in
# (src/ui/components/autorun-manager.c). Read x before the call instead.
#
# Usage: check-steal-order.py DIR...  (exits 1 and lists the calls if any)
#
# SPDX-License-Identifier: GPL-2.0-or-later

import pathlib
import re
import sys

STEAL = re.compile(r'g_steal_pointer\s*\(\s*&\s*([A-Za-z_]\w*(?:\s*(?:->|\.)\s*\w+)*)\s*\)')
KEYWORDS = {'if', 'while', 'for', 'switch', 'return', 'sizeof', 'G_LIKELY', 'G_UNLIKELY'}


def strip(text):
    """Blanks comments, strings and character literals, keeping offsets."""
    out = []
    i, n = 0, len(text)
    while i < n:
        if text.startswith('/*', i):
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r'[^\n]', ' ', text[i:j]))
            i = j
        elif text.startswith('//', i):
            j = text.find('\n', i)
            j = n if j < 0 else j
            out.append(' ' * (j - i))
            i = j
        elif text[i] in '"\'':
            quote, j = text[i], i + 1
            while j < n and text[j] != quote:
                j += 2 if text[j] == '\\' else 1
            j = min(j + 1, n)
            out.append(quote + re.sub(r'[^\n]', ' ', text[i + 1:j - 1]) + quote)
            i = j
        else:
            out.append(text[i])
            i += 1
    return ''.join(out)


def enclosing_calls(code, pos):
    """The (callee start, open paren, close paren) of the calls around pos."""
    calls = []
    depth = 0
    i = pos - 1
    while i >= 0:
        c = code[i]
        if c in ';{}' and depth == 0:
            break
        if c == ')':
            depth += 1
        elif c == '(':
            if depth > 0:
                depth -= 1
            else:
                # what is before the paren: a call, a keyword or a cast/group
                m = re.search(r'([A-Za-z_][\w\s>.\-]*)\s*$', code[:i])
                name = m.group(1).split()[-1] if m else ''
                word = re.split(r'->|\.', name)[-1] if name else ''
                if name and word not in KEYWORDS:
                    close, d = i, 0
                    for k in range(i, len(code)):
                        if code[k] == '(':
                            d += 1
                        elif code[k] == ')':
                            d -= 1
                            if d == 0:
                                close = k
                                break
                    calls.append((m.start(1), i, close))
                elif word in KEYWORDS:
                    break
        i -= 1
    return calls


def uses(text, lvalue):
    pattern = r'(?<![\w.>])' + r'\s*(?:->|\.)\s*'.join(map(re.escape, re.split(r'\s*(?:->|\.)\s*', lvalue))) + r'(?!\w)'
    return re.search(pattern, text) is not None


def check(path):
    code = strip(path.read_text(errors='replace'))
    found = []
    for m in STEAL.finditer(code):
        lvalue = m.group(1)
        for callee, open_paren, close in enclosing_calls(code, m.start()):
            text = code[callee:close + 1]
            rest = text[:m.start() - callee] + ' ' * len(m.group(0)) + text[m.end() - callee:]
            if uses(rest, lvalue):
                line = code.count('\n', 0, m.start()) + 1
                found.append(f'{path}:{line}: g_steal_pointer (&{lvalue}) and another read of {lvalue} in one call')
                break
    return found


def main():
    found = []
    for root in sys.argv[1:]:
        for path in sorted(pathlib.Path(root).rglob('*.c')):
            found += check(path)
    for line in found:
        print(line)
    return 1 if found else 0


if __name__ == '__main__':
    sys.exit(main())
