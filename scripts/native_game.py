#!/usr/bin/env python3
# A game as native code, on this Mac (plan.md, phases 3 and 4: the first game's main executable generated locally, never committed, and checked in lockstep along its
# route). From a disc and its profile (profiles/<serial>/game.toml, by the serial in the boot executable's name; its boot_sha256 must match): the host, with OpenBIOS
# as native code, runs the profile's route with the game interpreted and --capture, for what the game's code calls and jumps to, and RAM; tools/codegen.py writes the
# game's native code from the boot executable, that capture and the profile's [analysis] hints; the host is built with it (make -C shell/mac game) and runs the
# route again in lockstep with an interpreting machine, which must agree to the end, on the same picture. Then the route's time natively and interpreted. What it
# writes stays in shell/mac/build: the capture and the generated C hold the game's code, and are never committed, copied or shared (AGENTS.md: no game data).
# Usage: scripts/native_game.py <disc.cue | disc.bin> [--profile <game.toml>] [--route first_level] [--frames <n>] [--input <frame>:<hex-buttons> ...]
import argparse, hashlib, json, os, re, subprocess, sys, time, tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import disc, overlays, replay

MAC = ROOT / 'shell' / 'mac'
GEN = MAC / 'build' / 'gen'


def step(what, cmd, part='native_game'):   # runs a step, printing its command's own report lines; its output and the time it took
    print(f'{part}: {what}', file=sys.stderr)
    t = time.monotonic()
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
    for line in (r.stdout + r.stderr).splitlines():
        if line.startswith(('host:', 'capture:', 'codegen:', '[RED]', 'make: ***')) or ' error: ' in line: print(f'  {line}', file=sys.stderr)
    return r, time.monotonic() - t


def frame_hash(r): m = re.search(r'frame hash ([0-9a-f]{16})', r.stdout); return m.group(1) if m else None


def main():
    p = argparse.ArgumentParser(description="Generate a game's native code from its disc and check it in lockstep along its profile's route")
    p.add_argument('disc', type=Path, help='a CUE sheet, or a raw BIN of one data track')
    p.add_argument('--profile', type=Path, help="the game's profile (default: profiles/<serial>/game.toml)")
    p.add_argument('--route', default='validation', help="profile route table (default: validation; first_level for phase 4)")
    p.add_argument('--frames', type=int, help="the route's length in frames (default: the profile's [validation] frames)")
    p.add_argument('--input', action='append', help="<frame>:<hex-buttons>, as the host takes it (default: the profile's [validation] inputs)")
    a = p.parse_args()
    if a.frames is not None and a.frames < 1: p.error('--frames must be positive')
    if not a.disc.is_file(): raise SystemExit(f'{a.disc}: no such file')
    name, exe = disc.boot(disc.tracks(a.disc)[0])
    profile = a.profile or ROOT / 'profiles' / str(disc.serial(name)) / 'game.toml'
    if not profile.is_file(): raise SystemExit(f'native_game: no profile {profile.relative_to(ROOT) if profile.is_relative_to(ROOT) else profile}: give one with --profile')
    with open(profile, 'rb') as f: game = tomllib.load(f)
    want = game.get('disc', {}).get('boot_sha256')
    if want and hashlib.sha256(exe).hexdigest() != want: raise SystemExit(f'native_game: the disc\'s boot executable is not the one {profile.name} describes')
    v = game.get(a.route, {})
    frames, inputs = a.frames or v.get('frames'), a.input if a.input is not None else v.get('inputs', [])
    if not frames: raise SystemExit(f'native_game: no route: the profile has no [{a.route}] frames, and --frames is not given')
    try: inputs = replay.events(inputs, v.get('pulses', []) if a.input is None else (), frames)
    except (ValueError, KeyError) as e: raise SystemExit(f'native_game: invalid route: {e}') from e
    GEN.mkdir(parents=True, exist_ok=True); (GEN / 'route.inputs').write_text('\n'.join(inputs) + ('\n' if inputs else ''))
    route = ['--bios', ROOT / 'bios' / 'openbios' / 'openbios.bin', '--disc', a.disc, '--headless', '--frames', frames, '--input-file', GEN / 'route.inputs']

    r, _ = step('building the host, with OpenBIOS as native code', ['make', '-C', MAC])
    if r.returncode: raise SystemExit('native_game: the host does not build')
    r, t = step(f'running the route, {frames} frames, the game interpreted, capturing what it runs',
                [MAC / 'build' / 'muyiunative', *route, '--capture', GEN / 'game.cap', '--png', GEN / 'route.png'])
    if r.returncode or not frame_hash(r): raise SystemExit('native_game: the interpreted route did not finish')
    route_hash = frame_hash(r)
    if a.frames is None and a.input is None and v.get('frame_hash_fnv1a64') and route_hash != v['frame_hash_fnv1a64']:
        print(f'native_game: the route ends on {route_hash}, not on the profile\'s {v["frame_hash_fnv1a64"]}: the runtime has changed since the route was recorded. '
              f'Its last frame is in {GEN.relative_to(ROOT) / "route.png"} (keep it on this Mac); if it is still the profile\'s screen, record the new hash '
              'in the profile. The native code is checked against this run', file=sys.stderr)
    captured = overlays.report(overlays.read(GEN / 'game.cap.blocks'))
    (GEN / 'game.survey.json').write_text(json.dumps(captured, indent=2) + '\n')
    print(f'native_game: {captured["blocks"]} captured block versions; {captured["replaced_addresses"]} addresses hold multiple versions', file=sys.stderr)
    cmd = [sys.executable, ROOT / 'tools' / 'codegen.py', a.disc, '--capture', GEN / 'game.cap', '--name', 'game', '-o', GEN / 'game.c', '--profile', profile,
           '--max-ops', 20000]   # files clang compiles in parallel, a few seconds each, instead of one it may run out of memory on
    r, _ = step("generating the game's native code", cmd)
    if r.returncode: raise SystemExit(f'native_game: code generation failed: {r.stderr.strip()[-400:]}')
    parts = ' '.join(f'build/gen/{f.name}' for f in sorted(GEN.glob('game*.c')))
    r, _ = step('building the host with it', ['make', '-C', MAC, f'-j{os.cpu_count() or 4}', 'game', f'GAME_C={parts}'])
    if r.returncode: raise SystemExit('native_game: the game\'s host does not build')
    game_host = MAC / 'build' / 'game' / 'muyiunative'
    r, _ = step('running the route in lockstep with an interpreting machine', [game_host, *route, '--lockstep'])
    if r.returncode or 'the machines agree' not in r.stdout or frame_hash(r) != route_hash:
        raise SystemExit('native_game: the native code and the interpreter differ on the route (the [RED] line above says where)' if r.returncode == 3 else
                         f'native_game: the lockstep run failed (exit {r.returncode}, frame hash {frame_hash(r)})')
    r, native = step('timing the route natively', [game_host, *route])
    if r.returncode or frame_hash(r) != route_hash: raise SystemExit('native_game: the native timing run differs from the captured route')
    r, interpreted = step('timing it interpreted', [game_host, *route, '--interpret'])
    if r.returncode or frame_hash(r) != route_hash: raise SystemExit('native_game: the interpreted timing run differs from the captured route')
    print(f'native_game: the route agrees with the interpreter to the end, frame hash {route_hash}; natively {native:.1f} s, interpreted {interpreted:.1f} s')
    return 0


if __name__ == '__main__': sys.exit(main())
