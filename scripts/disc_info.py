#!/usr/bin/env python3
# What identifies a disc in its profile, profiles/<serial>/game.toml (plan.md, section 2: the profile lookup by serial and SHA-256): the serial in the name of the
# executable SYSTEM.CNF boots, the region of the licence string, and the SHA-256 of that executable and of each track. Printed as TOML to paste into the profile, with
# a line per step on stderr. The disc is only read (tools/disc.py), and nothing of it but these values is printed, not even its path.
# Usage: scripts/disc_info.py <disc.cue | disc.bin>
import argparse, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import disc


def main():
    p = argparse.ArgumentParser(description='Print what identifies a PS1 disc, as TOML for its profile.')
    p.add_argument('disc', type=Path, help='a CUE sheet, or a raw BIN of one data track (2352-byte sectors)')
    a = p.parse_args()
    if not a.disc.is_file(): raise SystemExit(f'{a.disc}: no such file')
    print('disc_info: reading the disc', file=sys.stderr)
    d = disc.identify(a.disc)
    print(f'disc_info: {len(d["tracks"])} track(s) hashed', file=sys.stderr)
    print('# From scripts/disc_info.py, for profiles/<serial>/game.toml')
    print(f'serial = "{d["serial"]}"' if d['serial'] else f"# no serial: the boot executable's name, {d['boot']}, holds none")
    print('\n[disc]')
    print(f'licence = "{d["licence"]}"' if d['licence'] else '# no licence string: a console without a modchip refuses the disc')
    print(f"boot = '{d['boot']}'\nboot_sha256 = \"{d['boot_sha256']}\"\ntracks = [")
    for t in d['tracks']: print(f'  {{ number = {t["number"]}, mode = "{t["mode"]}", sectors = {t["sectors"]}, sha256 = "{t["sha256"]}" }},')
    print(']')


if __name__ == '__main__': main()
