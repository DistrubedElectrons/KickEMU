#!/usr/bin/env python3
"""Fail when Kconfig dropped or changed a requested option.

Usage: check-config.py <defconfig> <fragment> <resolved .config>

The request is the defconfig with the fragment applied on top (later lines win, as in
Kconfig). olddefconfig silently drops options whose dependencies are not met, so every
requested line is compared with the resolved configuration:

  BR2_X=value       must appear exactly so in .config
  # BR2_X is not set  must not be enabled: "# BR2_X is not set" or absent (absent is how
                    Kconfig writes string/hex options whose dependencies are off)
"""
import re
import sys

SET = re.compile(r"^(BR2_\w+)=(.*)$")
UNSET = re.compile(r"^# (BR2_\w+) is not set$")


def parse(path):
    """Ordered option -> value (None for 'is not set') of a config file."""
    out = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            m = SET.match(line)
            if m:
                out[m.group(1)] = m.group(2)
                continue
            m = UNSET.match(line)
            if m:
                out[m.group(1)] = None
    return out


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    defconfig, fragment, resolved_path = sys.argv[1:]
    request = parse(defconfig)
    request.update(parse(fragment))
    resolved = parse(resolved_path)

    bad = []
    for key, want in request.items():
        have = resolved.get(key, "<absent>")
        if want is None:
            if have not in (None, "<absent>"):
                bad.append(f"{key}: requested not set, resolved {key}={have}")
        elif have != want:
            shown = "is not set" if have is None else have
            bad.append(f"{key}: requested {want}, resolved {shown}")

    print(f"checked {len(request)} requested options against {resolved_path}")
    if bad:
        print(f"{len(bad)} requested options did not survive olddefconfig:")
        for b in bad:
            print("  " + b)
        sys.exit(1)
    print("all requested options are in the resolved configuration")


if __name__ == "__main__":
    main()
