#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate the i.MX8MM CCM clock root table (imx8mm_ccm_roots.c.inc).

The slice offsets and the 8 mux inputs of every clock root are taken from
drivers/clk/imx/clk-imx8mm.c of the vendor kernel, which mirrors the
"Clock Root Selects" table of the i.MX 8M Mini Reference Manual.

Usage: gen-imx8mm-clock-roots.py <linux>/drivers/clk/imx/clk-imx8mm.c > imx8mm_ccm_roots.c.inc
"""
import re
import sys

src = open(sys.argv[1]).read()

sels = {}
for m in re.finditer(r'static const char \*(imx8mm_\w+_sels)\[\]\s*=\s*\{(.*?)\};', src, re.S):
    sels[m.group(1)] = re.findall(r'"(\w+)"', m.group(2))

roots = []  # (offset, name, kind, parents)
for m in re.finditer(r'imx8m_clk_(?:hw_)?composite(?:_critical|_bus|_core)?\("(\w+)",\s*(imx8mm_\w+_sels),\s*base \+ (0x[0-9a-f]+)\)', src):
    roots.append((int(m.group(3), 16), m.group(1), 'IMX8MM_ROOT_COMPOSITE', sels[m.group(2)]))
for m in re.finditer(r'imx_clk_mux2\("(\w+)_src",\s*base \+ (0x[0-9a-f]+), 24, 3,\s*(imx8mm_\w+_sels)', src):
    roots.append((int(m.group(2), 16), m.group(1), 'IMX8MM_ROOT_CORE', sels[m.group(3)]))

# GPT2..GPT6 are not used by Linux but exist in the RM with the GPT1 parent list.
gpt1 = [r for r in roots if r[1] == 'gpt1'][0]
for i in range(2, 7):
    roots.append((gpt1[0] + (i - 1) * 0x80, 'gpt%d' % i, gpt1[2], gpt1[3]))

roots.append((0x9080, 'ipg_root', 'IMX8MM_ROOT_IPG', ['ahb']))
roots.append((0x9180, 'ipg_audio_root', 'IMX8MM_ROOT_IPG', ['audio_ahb']))
roots.append((0x9800, 'dram_core', 'IMX8MM_ROOT_MUX1', ['dram_pll_out', 'dram_alt_root']))
roots.append((0x9880, 'arm_a53_core', 'IMX8MM_ROOT_MUX1', ['arm_a53_div', 'arm_pll_out']))
roots.sort()

names = {r[1]: i for i, r in enumerate(roots)}
alias = {'arm_a53_div': 'arm_a53', 'arm_m4_div': 'arm_m4', 'vpu_pll': 'vpu_pll_out',
         'dram_alt_root': 'dram_alt'}

def parent(p):
    q = alias.get(p, p)
    if q in names:
        ref = 'IMX8MM_CLK_ROOT(%d)' % names[q]
        if p == 'dram_alt_root':
            ref = 'IMX8MM_CLK_ROOT_DIV4(%d)' % names[q]
        return ref
    return 'IMX8MM_SRC_' + q.upper()

out = []
out.append('/* Generated from the i.MX8MM clock slice list, do not edit by hand. */')
out.append('static const IMX8MMClockRoot imx8mm_clock_roots[] = {')
for off, name, kind, ps in roots:
    pl = ', '.join(parent(p) for p in ps)
    out.append('    { 0x%04x, "%s", %s, { %s } },' % (off, name, kind, pl))
out.append('};')
print('\n'.join(out))
srcs = set()
for _, _, _, ps in roots:
    for p in ps:
        if alias.get(p, p) not in names:
            srcs.add(p)
print('/* sources: %s */' % ' '.join(sorted(srcs)), file=sys.stderr)
