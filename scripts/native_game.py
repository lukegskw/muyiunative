#!/usr/bin/env python3
# Native route acceptance and timing, using the same multi-route capture/compile/coverage gates as the Mac app build (tools/mac_build.py). Captures and generated
# code stay in independent ignored workspaces; the retained report contains addresses/counters only. No app is packed and no existing game or save is replaced.
import argparse, hashlib, re, subprocess, sys, tempfile, time, tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import disc, mac_build

MAC = ROOT / 'shell' / 'mac'


def step(what, cmd, part='native_game'):   # runs a step, printing its command's own report lines; its output and the time it took
    print(f'{part}: {what}', file=sys.stderr)
    t = time.monotonic()
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
    for line in (r.stdout + r.stderr).splitlines():
        if line.startswith(('host:', 'capture:', 'codegen:', '[RED]', 'make: ***')) or ' error: ' in line: print(f'  {line}', file=sys.stderr)
    return r, time.monotonic() - t


def frame_hash(result):
    match = re.search(r'frame hash ([0-9a-f]{16})', result.stdout); return match[1] if match else None


def main():
    p = argparse.ArgumentParser(description="Capture, compile and verify native game routes, then compare native and interpreted timings")
    p.add_argument('disc', type=Path); p.add_argument('--profile', type=Path)
    p.add_argument('--route', action='append', help='profile route to capture (repeatable; default: native.routes, then validation)')
    p.add_argument('--frames', type=int); p.add_argument('--input', action='append')
    p.add_argument('--min-native-percent', type=float); p.add_argument('--strict-lockstep', action='store_true')
    p.add_argument('--check-metal', type=int, choices=[1, 2, 4], action='append', help='also verify GPU-only drawing at this scale (repeatable; needs Metal)')
    p.add_argument('--card-seed', type=Path, help='folder with initial card1.mcr/card2.mcr; originals are never modified')
    a = p.parse_args()
    if a.frames is not None and a.frames < 1: raise SystemExit('native_game: --frames must be positive')
    name, image = disc.boot(disc.tracks(a.disc)[0]); serial = disc.serial(name)
    profile = a.profile or ROOT / 'profiles' / str(serial) / 'game.toml'
    if not profile.is_file(): raise SystemExit(f'native_game: no profile profiles/{serial}/game.toml; supply one with --profile')
    game = tomllib.loads(profile.read_text()); disc_info = game.get('disc', {}); policy = game.get('native', {})
    if not isinstance(disc_info, dict) or not isinstance(policy, dict): raise SystemExit('native_game: disc and native profile entries must be tables')
    want = disc_info.get('boot_sha256')
    if want and hashlib.sha256(image).hexdigest() != want: raise SystemExit(f"native_game: the disc's boot executable is not the one {profile.name} describes")
    routes = a.route or policy.get('routes', ['validation'])
    if not isinstance(routes, list) or not routes or any(not isinstance(route, str) for route in routes): raise SystemExit('native_game: invalid native routes')
    for route in routes:
        data = game.get(route, {})
        if not isinstance(data, dict) or (not a.frames and not data.get('frames')):
            raise SystemExit(f'native_game: no route: the profile has no [{route}] frames, and --frames is not given')
    plans = mac_build.build_routes(a.disc, profile, routes, a.frames, a.input, a.min_native_percent, mac_build.read_card_seed(a.card_seed))
    folder = MAC / 'build/native'; folder.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix=f'{serial or "homebrew"}-', dir=folder))
    records, report = mac_build.compile_routes([a.disc], [plans], work, a.strict_lockstep)
    if a.check_metal: mac_build.check_metal(records, work, a.check_metal)
    for record in records:
        route, plan = record['command'], record['plan']
        with mac_build.route_cards(plan) as cards: r, native = step('timing the route natively', [work / 'game/muyiunative', *route, *cards])
        if r.returncode or mac_build.completed(r.stdout, plan) != record['expected']: raise SystemExit('native_game: the native timing run differs from capture')
        with mac_build.route_cards(plan) as cards: r, interpreted = step('timing it interpreted', [work / 'game/muyiunative', *route, *cards, '--interpret'])
        if r.returncode or mac_build.completed(r.stdout, plan) != record['expected']: raise SystemExit('native_game: the interpreted timing run differs from capture')
        print(f'native_game: {plan["route"]} agrees with the interpreter; frame hash {record["expected"]}; natively {native:.1f} s, interpreted {interpreted:.1f} s')
    print(f'native_game: native coverage and fallback report: {report}')
    return 0


if __name__ == '__main__':
    try: sys.exit(main())
    except (OSError, ValueError) as e: raise SystemExit(f'native_game: {e}') from e
