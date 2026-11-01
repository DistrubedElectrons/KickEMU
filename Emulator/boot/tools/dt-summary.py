#!/usr/bin/env python3
#
# Summarise a flattened device tree (.dtb) as Markdown: every enabled node with a
# compatible string, its registers, interrupts (resolved to GIC SPI numbers or GPIO
# lines), clocks (resolved to IMX8MM_CLK_* names), GPIOs, DMA channels and I2C/SPI
# addresses, plus aliases, chosen, memory, CPUs and PSCI.
#
# Usage: dt-summary.py <board.dtb> <imx8mm-clock.h> [title]
#
# Self-contained FDT parser (no libfdt needed), so it runs in any Python 3.

import re
import struct
import sys

FDT_BEGIN_NODE, FDT_END_NODE, FDT_PROP, FDT_NOP, FDT_END = 1, 2, 3, 4, 9


class Node:
    def __init__(self, name, parent):
        self.name = name
        self.parent = parent
        self.props = {}
        self.children = []

    @property
    def path(self):
        if self.parent is None:
            return "/"
        p = self.parent.path
        return (p if p != "/" else "") + "/" + self.name

    def cells(self, prop):
        v = self.props.get(prop)
        if v is None:
            return None
        return list(struct.unpack(">%dI" % (len(v) // 4), v[: len(v) // 4 * 4]))

    def u32(self, prop, default=None):
        c = self.cells(prop)
        return c[0] if c else default

    def strings(self, prop):
        v = self.props.get(prop)
        if v is None:
            return []
        return [s.decode(errors="replace") for s in v.rstrip(b"\0").split(b"\0")]

    def string(self, prop):
        s = self.strings(prop)
        return s[0] if s else None

    def enabled(self):
        n = self
        while n is not None:
            st = n.string("status")
            if st is not None and st not in ("okay", "ok"):
                return False
            n = n.parent
        return True


def parse_fdt(data):
    magic, totalsize, off_struct, off_strings = struct.unpack(">IIII", data[:16])
    if magic != 0xD00DFEED:
        raise SystemExit("not a DTB")
    pos = off_struct
    root = None
    cur = None
    while True:
        (tok,) = struct.unpack(">I", data[pos : pos + 4])
        pos += 4
        if tok == FDT_BEGIN_NODE:
            end = data.index(b"\0", pos)
            name = data[pos:end].decode()
            pos = (end + 4) & ~3
            node = Node(name, cur)
            if cur is None:
                root = node
            else:
                cur.children.append(node)
            cur = node
        elif tok == FDT_END_NODE:
            cur = cur.parent
        elif tok == FDT_PROP:
            ln, nameoff = struct.unpack(">II", data[pos : pos + 8])
            pos += 8
            nend = data.index(b"\0", off_strings + nameoff)
            pname = data[off_strings + nameoff : nend].decode()
            cur.props[pname] = data[pos : pos + ln]
            pos = (pos + ln + 3) & ~3
        elif tok == FDT_NOP:
            continue
        elif tok == FDT_END:
            break
        else:
            raise SystemExit("bad FDT token %d" % tok)
    return root


def walk(node):
    yield node
    for c in node.children:
        yield from walk(c)


def load_clock_names(header):
    names = {}
    with open(header) as f:
        for line in f:
            m = re.match(r"#define\s+(IMX8MM_\w+)\s+(\d+)\b", line)
            if m and not m.group(1).endswith("_END"):
                names.setdefault(int(m.group(2)), m.group(1))
    return names


class Tree:
    def __init__(self, root, clock_names):
        self.root = root
        self.clock_names = clock_names
        self.by_phandle = {}
        self.gpio_index = {}
        for n in walk(root):
            if n.path == "/aliases":
                for k in n.props:
                    m = re.match(r"gpio(\d+)$", k)
                    if m:
                        self.gpio_index[n.string(k)] = int(m.group(1)) + 1
        for n in walk(root):
            ph = n.u32("phandle") or n.u32("linux,phandle")
            if ph:
                self.by_phandle[ph] = n

    def gpio_name(self, ctrl, pin):
        """GPIO5 pin 4 -> GPIO5_IO04 (i.MX naming); other controllers by node name."""
        idx = self.gpio_index.get(ctrl.path)
        if idx is not None:
            return "GPIO%d_IO%02d" % (idx, pin)
        return "%s.%d" % (ctrl.name, pin)

    # Registers -----------------------------------------------------------------
    def reg(self, node):
        cells = node.cells("reg")
        if not cells or node.parent is None:
            return ""
        ac = node.parent.u32("#address-cells", 2)
        sc = node.parent.u32("#size-cells", 1)
        out = []
        step = ac + sc
        for i in range(0, len(cells) - step + 1, step):
            addr = 0
            for c in cells[i : i + ac]:
                addr = (addr << 32) | c
            size = 0
            for c in cells[i + ac : i + step]:
                size = (size << 32) | c
            if sc == 0:
                out.append("0x%x" % addr)
            else:
                out.append("0x%08x+0x%x" % (addr, size))
        names = node.strings("reg-names")
        if names and len(names) == len(out):
            out = ["%s %s" % (n, r) for n, r in zip(names, out)]
        return ", ".join(out)

    # Interrupts ----------------------------------------------------------------
    def irq_parent(self, node):
        n = node
        while n is not None:
            ph = n.u32("interrupt-parent")
            if ph:
                return self.by_phandle.get(ph)
            n = n.parent
        return None

    def fmt_irq(self, ctrl, spec):
        trig = {1: "rising", 2: "falling", 3: "both", 4: "high", 8: "low"}
        compat = ctrl.string("compatible") or ""
        if "gic" in compat:
            kind, num, flags = (spec + [0, 0, 0])[:3]
            if kind == 0:
                return "SPI %d (GIC %d) %s" % (num, num + 32, trig.get(flags & 0xF, hex(flags)))
            return "PPI %d %s" % (num, trig.get(flags & 0xF, hex(flags)))
        if spec:
            flags = spec[1] if len(spec) > 1 else 0
            return "%s %s" % (self.gpio_name(ctrl, spec[0]), trig.get(flags & 0xF, hex(flags)))
        return ctrl.name

    def interrupts(self, node):
        out = []
        ext = node.cells("interrupts-extended")
        if ext:
            i = 0
            while i < len(ext):
                ctrl = self.by_phandle.get(ext[i])
                n = ctrl.u32("#interrupt-cells", 1) if ctrl else 1
                out.append(self.fmt_irq(ctrl, ext[i + 1 : i + 1 + n]) if ctrl else "?")
                i += 1 + n
            return "; ".join(out)
        cells = node.cells("interrupts")
        if not cells:
            return ""
        ctrl = self.irq_parent(node)
        if ctrl is None:
            return " ".join(map(str, cells))
        n = ctrl.u32("#interrupt-cells", 1)
        for i in range(0, len(cells), n):
            out.append(self.fmt_irq(ctrl, cells[i : i + n]))
        names = node.strings("interrupt-names")
        if names and len(names) == len(out):
            out = ["%s: %s" % (a, b) for a, b in zip(names, out)]
        return "; ".join(out)

    # Phandle lists with arguments (clocks, gpios, dmas, pwms) ------------------
    def phandle_list(self, node, prop, cells_prop):
        cells = node.cells(prop)
        if not cells:
            return []
        out = []
        i = 0
        while i < len(cells):
            target = self.by_phandle.get(cells[i])
            if target is None:
                out.append(("?", cells[i + 1 :]))
                break
            n = target.u32(cells_prop, 0)
            out.append((target, cells[i + 1 : i + 1 + n]))
            i += 1 + n
        return out

    def clocks(self, node):
        out = []
        for target, args in self.phandle_list(node, "clocks", "#clock-cells"):
            if target == "?":
                out.append("?")
            elif args and "imx8mm-ccm" in (target.string("compatible") or ""):
                out.append(self.clock_names.get(args[0], "clk %d" % args[0]))
            elif args:
                out.append("%s %s" % (target.name, " ".join(map(str, args))))
            else:
                out.append(target.name)
        names = node.strings("clock-names")
        if names and len(names) == len(out):
            out = ["%s=%s" % (a, b) for a, b in zip(names, out)]
        return ", ".join(out)

    def gpios(self, node):
        out = []
        for prop in sorted(node.props):
            if prop.endswith("-gpios") or prop in ("gpios", "gpio") or prop.endswith("-gpio"):
                for target, args in self.phandle_list(node, prop, "#gpio-cells"):
                    if target == "?":
                        continue
                    pol = "active-low" if len(args) > 1 and args[1] & 1 else "active-high"
                    out.append("%s=%s %s" % (prop, self.gpio_name(target, args[0] if args else -1), pol))
        return out

    def dmas(self, node):
        out = []
        for target, args in self.phandle_list(node, "dmas", "#dma-cells"):
            if target != "?":
                out.append("%s(%s)" % (target.name, " ".join(map(str, args))))
        names = node.strings("dma-names")
        if names and len(names) == len(out):
            out = ["%s=%s" % (a, b) for a, b in zip(names, out)]
        return out


    def graph_links(self):
        """(device, device) for every remote-endpoint; a device is the nearest node with
        a compatible string."""
        def owner(n):
            while n is not None and "compatible" not in n.props:
                n = n.parent
            return n
        links = []
        for n in walk(self.root):
            ph = n.u32("remote-endpoint")
            if ph and ph in self.by_phandle:
                a, b = owner(n), owner(self.by_phandle[ph])
                if a is not None and b is not None and a.enabled() and b.enabled():
                    links.append((a.path, b.path))
        return links


def md_escape(s):
    return s.replace("|", "\\|")


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__ if __doc__ else "usage: dt-summary.py <dtb> <clock header> [title]")
    dtb, header = sys.argv[1], sys.argv[2]
    title = sys.argv[3] if len(sys.argv) > 3 else dtb
    with open(dtb, "rb") as f:
        root = parse_fdt(f.read())
    t = Tree(root, load_clock_names(header))
    p = print

    p("## Generated node table: %s" % title)
    p()
    p("Model: `%s`, compatible: `%s`" % (root.string("model"), ", ".join(root.strings("compatible"))))
    p()

    def find(path):
        for n in walk(root):
            if n.path == path:
                return n
        return None

    aliases = find("/aliases")
    if aliases:
        p("### Aliases")
        p()
        p("| alias | node |")
        p("|---|---|")
        for k in sorted(aliases.props):
            p("| %s | `%s` |" % (k, aliases.string(k)))
        p()
    chosen = find("/chosen")
    if chosen:
        p("### /chosen")
        p()
        for k in sorted(chosen.props):
            p("- `%s` = `%s`" % (k, chosen.string(k)))
        p()

    mem = [n for n in walk(root) if n.string("device_type") == "memory"]
    if mem:
        p("### Memory")
        p()
        for n in mem:
            p("- `%s`: %s" % (n.path, t.reg(n)))
        rsv = find("/reserved-memory")
        if rsv is not None:
            kids = [c for c in rsv.children if c.enabled()]
            p("- `/reserved-memory`: %s" % (", ".join("`%s` %s" % (c.name, t.reg(c)) for c in kids) or "no regions"))
        p()

    links = t.graph_links()
    if links:
        p("### Graph links (ports/endpoints)")
        p()
        seen = set()
        for a, b in links:
            if (b, a) in seen or (a, b) in seen:
                continue
            seen.add((a, b))
            p("- `%s` <-> `%s`" % (a, b))
        p()

    p("### Enabled nodes")
    p()
    p("Interrupts: `SPI n (GIC m)` is the GIC shared peripheral interrupt n, i.e. GIC INTID m.")
    p("Clocks are the `IMX8MM_CLK_*` indices of the CCM node. Nodes are listed in tree order;")
    p("devices sharing one interrupt or address range appear separately.")
    p()
    p("| node | compatible | reg | interrupts | clocks | other |")
    p("|---|---|---|---|---|---|")
    count = 0
    for n in walk(root):
        if not n.enabled():
            continue
        if "compatible" not in n.props:
            # Children without a compatible that still describe wiring (gpio-keys
            # buttons, LEDs) are listed with their GPIOs.
            g = t.gpios(n)
            if g and n.parent is not None and "compatible" in n.parent.props:
                extra = []
                if n.string("label"):
                    extra.append("label=%s" % n.string("label"))
                if "linux,code" in n.props:
                    extra.append("linux,code=%d" % n.u32("linux,code"))
                p("| `%s` | (child of %s) |  |  |  | %s |" % (
                    n.path, md_escape(n.parent.string("compatible")), md_escape("; ".join(g + extra))))
            continue
        count += 1
        other = t.gpios(n) + t.dmas(n)
        if n.parent is not None and n.parent.u32("#size-cells", 1) == 0 and n.cells("reg"):
            bus = n.parent.name
            other.insert(0, "on %s" % bus)
        for k in ("method", "enable-method", "cpu-release-addr", "fsl,sai-synchronous-rx",
                  "fsl,sai-mclk-direction-output", "bus-width", "non-removable", "phy-mode",
                  "touchscreen-size-x", "touchscreen-size-y", "assigned-clock-rates"):
            if k in n.props:
                v = n.props[k]
                if not v:
                    other.append(k)
                elif all(32 <= b < 127 for b in v.rstrip(b"\0")) and v.endswith(b"\0"):
                    other.append("%s=%s" % (k, n.string(k)))
                else:
                    other.append("%s=%s" % (k, " ".join(str(c) for c in n.cells(k))))
        p("| `%s` | %s | %s | %s | %s | %s |" % (
            n.path,
            md_escape(", ".join(n.strings("compatible"))),
            md_escape(t.reg(n)),
            md_escape(t.interrupts(n)),
            md_escape(t.clocks(n)),
            md_escape("; ".join(other)),
        ))
    p()
    p("%d enabled nodes with a compatible string." % count)


if __name__ == "__main__":
    main()
