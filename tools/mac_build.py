#!/usr/bin/env python3
# Phase 5's Mac product build, called by scripts/muyiunative: capture a route, compile its observed code, check it in lockstep, then pack a local game app. Disc files
# and generated code stay in ignored build folders or the user's output app. Each build owns its workspace; a set freezes every disc's code versions into one binary.
import argparse, hashlib, plistlib, re, shutil, subprocess, sys, tempfile
from pathlib import Path

import analyze, codegen, disc, disc_set, overlays, replay
from pack_app import packed_id
from profile_toml import tomllib

ROOT = Path(__file__).resolve().parents[1]
MAC = ROOT / 'shell/mac'


def build_plan(source, profile=None, route=None, frames=None, inputs=None) -> dict:
    name, image = disc.boot(disc.tracks(source)[0]); serial = disc.serial(name); digest = hashlib.sha256(image).hexdigest()
    candidate = ROOT / 'profiles' / str(serial) / 'game.toml'
    profile = Path(profile).resolve() if profile is not None else candidate if candidate.is_file() else None
    game = tomllib.loads(profile.read_text()) if profile else {}
    if profile and (not isinstance(game.get('disc'), dict) or game['disc'].get('boot_sha256') != digest): raise SystemExit(f'build: the disc\'s boot executable is not the one {profile.name} describes')
    selected = route or ('first_level' if 'first_level' in game else 'validation')
    if route and route not in game: raise SystemExit(f'build: no [{route}] route in the profile')
    v = game.get(selected, {})
    if not isinstance(v, dict): raise SystemExit(f'build: [{selected}] must be a route table')
    count = frames if frames is not None else v.get('frames', 2400)
    if type(count) is not int or count < 1: raise SystemExit('build: --frames must be positive')
    try: events = replay.events(inputs if inputs is not None else v.get('inputs', []), v.get('pulses', []) if inputs is None else (), count)
    except (ValueError, KeyError, TypeError, AttributeError) as e: raise SystemExit(f'build: invalid route: {e}') from e
    title = game.get('name') or serial or Path(source).stem
    if not isinstance(title, str): raise SystemExit('build: the profile name must be a string')
    title = re.sub(r'[/\x00-\x1f:]', '-', title).strip() or 'PS1 Game'
    if len(title.encode()) >= 256: raise SystemExit('build: the game name must be shorter than 256 UTF-8 bytes')
    return dict(serial=serial, boot_sha256=digest, title=title, profile=profile, route=selected, frames=count, inputs=events,
                save_id=f'{serial or "homebrew"}-{digest}')


def bundle_disc(source, dest):
    ts = disc.tracks(source); dest = Path(dest); dest.mkdir(parents=True, exist_ok=False)
    files, lines, previous = {}, [], None
    for t in ts:
        src = t.file.resolve()
        if src not in files:
            files[src] = f'file{len(files) + 1:02d}.bin'; shutil.copyfile(src, dest / files[src])
        if not lines or src != previous: lines.append(f'FILE "{files[src]}" BINARY')
        mode = t.mode
        if Path(source).suffix.lower() != '.cue':
            with src.open('rb') as f: header = f.read(16)
            if len(header) == 16 and header[15] == 1: mode = 'MODE1/2352'
        lines.append(f'  TRACK {t.number:02d} {mode}')
        for index, at in ([(0, t.first)] if t.first < t.start else []) + [(1, t.start)]:
            lines.append(f'    INDEX {index:02d} {at // 4500:02d}:{at // 75 % 60:02d}:{at % 75:02d}')
        previous = src
    cue = dest / 'disc.cue'; cue.write_text('\n'.join(lines) + '\n'); return cue


def run(label, command):
    print(f'build: {label}', flush=True)
    lines = []
    with subprocess.Popen([str(x) for x in command], cwd=MAC, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace') as process:
        assert process.stdout is not None
        for line in process.stdout:
            print(line, end='', flush=True); lines.append(line)
        result = process.wait()
    if result: raise SystemExit(f'build: {label} failed ({result})')
    return ''.join(lines)


def frame_hash(output):
    m = re.search(r'frame hash ([0-9a-f]{16})', output)
    if not m: raise SystemExit('build: the host did not report a frame hash')
    return m[1]


def build(a):
    sources = disc_set.sources(a.disc, getattr(a, 'extra_disc', None) or ())
    if a.renderer == 'software' and a.scale != 1: raise SystemExit('build: --scale 2 or 4 needs --renderer metal')
    plans = [build_plan(s, a.profile if i == 0 else None, a.route, a.frames, a.input) for i, s in enumerate(sources)]
    regions = {disc.licence(disc.tracks(s)[0]) for s in sources}
    if len(regions) != 1: raise SystemExit('build: all discs must have the same licence region')
    p = plans[0]
    out = (a.output or MAC / 'build/apps' / f'{p["title"]}.app').resolve()
    if out.suffix != '.app': raise SystemExit('build: --output must end in .app')
    bundle_id = f'org.muyiunative.game.{p["save_id"].lower()}'
    if out.exists() and packed_id(out) != bundle_id: raise SystemExit('build: the output exists and is not this game app; choose another --output')
    if out.is_relative_to(ROOT) and not out.is_relative_to(MAC / 'build'): raise SystemExit('build: game apps inside this repository must stay in shell/mac/build')
    # An app can be built outside the checkout, but may not replace its input disc or contain it while pack_app replaces an old bundle.
    if a.disc.resolve().is_relative_to(out) or any(s.is_relative_to(out) or any(t.file.resolve().is_relative_to(out) for t in disc.tracks(s)) for s in sources):
        raise SystemExit('build: the output app contains a source disc or playlist')
    products = MAC / 'build/products'; products.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix=f'{p["serial"] or "homebrew"}-', dir=products)); b = work.relative_to(MAC); gen = work / 'gen'; gen.mkdir()
    print(f'build: {p["title"]}; {len(sources)} disc(s)', flush=True)
    print(f'build: local build files in {work}', flush=True)
    make = ['make', f'B={b}', f'PYTHON={sys.executable}', '-j3']
    if (ROOT / 'build-kit.json').is_file(): make += ['PREBUILT_METALLIB=../../gfx/metal/ps1gpu.metallib']
    run('building the capture host', [*make, 'host'])
    routes, captures, programs = [], [], []
    for i, (source, plan) in enumerate(zip(sources, plans), 1):
        print(f'build: disc {i}/{len(sources)}; route {plan["route"]}, {plan["frames"]} frames', flush=True)
        inputs = gen / f'disc{i}.inputs'; inputs.write_text('\n'.join(plan['inputs']) + ('\n' if plan['inputs'] else ''))
        route = ['--bios', ROOT / 'bios/openbios/openbios.bin', '--disc', source, '--headless', '--frames', plan['frames'], '--input-file', inputs]
        capture = gen / f'disc{i}.cap'; routes.append(route)
        captures.append(run('capturing interpreted game code', [work / 'muyiunative', *route, '--capture', capture]))
        hints = analyze.hints_from_profile(plan['profile']) if plan['profile'] else None
        # Analyze each boot executable as well as the observed RAM/overlays: a later disc may boot only to a request for a memory-card save.
        for observed in (None, capture):
            word, entries = codegen.load(source, capture=observed); program = analyze.analyze(word, entries, hints)
            if observed: overlays.extend(program, overlays.read(Path(f'{capture}.blocks')))
            programs.append((word, program))
    print('build: generating native code', flush=True)
    merged = codegen.merge(programs)
    generated = codegen.generate_split(None, merged, 'game', 20000)
    for i, text in enumerate(generated):
        (gen / ('game.c' if i == len(generated) - 1 else f'game_{i + 1}.c')).write_text(text)
    parts = ' '.join(str(f.relative_to(MAC)) for f in sorted(gen.glob('game*.c')))
    run('compiling native game code', [*make, 'game', f'GAME_C={parts}'])
    for i, (route, captured) in enumerate(zip(routes, captures), 1):
        print(f'build: verifying disc {i}/{len(sources)}', flush=True)
        native = run('checking the route against the interpreter', [work / 'game/muyiunative', *route, '--lockstep'])
        if 'the machines agree' not in native or frame_hash(native) != frame_hash(captured): raise SystemExit('build: native code differs from the interpreter; no app was packed')
    resources = work / 'resources'; resources.mkdir(); catalog = []
    for i, (source, plan) in enumerate(zip(sources, plans), 1):
        target = 'disc' if i == 1 else f'discs/disc{i:02d}'
        bundle_disc(source, resources / target)
        catalog.append({'Title': f'Disc {i}' + (f' ({plan["serial"]})' if plan['serial'] else ''), 'Disc': f'{target}/disc.cue',
                        'Serial': plan['serial'] or '', 'BootSHA256': plan['boot_sha256']})
    manifest = {'Version': 2 if len(sources) > 1 else 1, 'Title': p['title'], 'Serial': p['serial'] or '', 'BootSHA256': p['boot_sha256'], 'SaveID': p['save_id'], 'Disc': 'disc/disc.cue',
                'Renderer': a.renderer, 'RenderScale': a.scale}
    if len(sources) > 1: manifest['Discs'] = catalog
    (resources / 'game.plist').write_bytes(plistlib.dumps(manifest))
    command = [sys.executable, ROOT / 'tools/pack_app.py', out, '--exe', work / 'game/muyiunative', '--name', p['title'],
        '--bundle-id', bundle_id, '--resource', ROOT / 'bios/openbios', '--resource', resources / 'disc', '--resource', resources / 'game.plist', '--resource', gen / 'ps1gpu.metallib']
    if len(sources) > 1: command += ['--resource', resources / 'discs']
    run('packing and signing the game app', command)
    print(f'build: app ready: {out}\nbuild: open it in Finder to play; keep this app and its game data on your Mac', flush=True)
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(prog='muyiunative', description='Build a native Mac app from your own PS1 disc')
    sub = ap.add_subparsers(dest='command', required=True); p = sub.add_parser('build', help='capture, compile, check and pack a game')
    p.add_argument('disc', type=Path, help='first CUE/BIN, or an ordered M3U playlist'); p.add_argument('--disc', dest='extra_disc', type=Path, action='append', help='additional disc in order (repeatable)')
    p.add_argument('--platform', choices=['mac-app'], default='mac-app')
    p.add_argument('--output', type=Path, help='output .app (default: shell/mac/build/apps/<game>.app)')
    p.add_argument('--profile', type=Path); p.add_argument('--route', help='profile route (default: first_level, then validation)')
    p.add_argument('--renderer', choices=['software', 'metal'], default='software', help='renderer when opening the game app (default: software)')
    p.add_argument('--scale', type=int, choices=[1, 2, 4], default=1, help='Metal internal resolution multiplier (default: 1)')
    p.add_argument('--frames', type=int, help='capture frames (unknown games default to 2400)'); p.add_argument('--input', action='append', help='frame:hex-buttons (repeatable)')
    a = ap.parse_args(argv)
    try: return build(a)
    except (OSError, ValueError, tomllib.TOMLDecodeError) as e: raise SystemExit(f'build: {e}') from e
