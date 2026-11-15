#!/usr/bin/env python3
# Phase 5's Mac product build, called by scripts/muyiunative: capture a route, compile its observed code, check it in lockstep, then pack a local game app. Disc files
# and generated code stay in ignored build folders or the user's output app. Each build owns its workspace; a set freezes every disc's code versions into one binary.
import argparse, hashlib, json, plistlib, re, shutil, subprocess, sys, tempfile
from contextlib import contextmanager
from pathlib import Path

import analyze, codegen, disc, disc_set, exe, native_coverage, overlays, replay
from pack_app import packed_id
from profile_toml import tomllib

ROOT = Path(__file__).resolve().parents[1]
MAC = ROOT / 'shell/mac'


def read_card_seed(folder):
    if folder is None: return None
    folder = Path(folder).resolve()
    if folder.is_relative_to(ROOT): raise ValueError('memory-card seeds must stay outside the checkout')
    cards = {name: (folder / name).read_bytes() for name in ('card1.mcr', 'card2.mcr')}
    if any(len(data) != 128 * 1024 for data in cards.values()): raise ValueError('each memory-card seed must contain exactly 128 KB')
    return cards


@contextmanager
def route_cards(plan):
    # Replaying against a directory modified by capture would compare different initial games. Every run gets the same immutable in-memory seed in a fresh
    # external temporary directory. Only hashes enter the report; cards never enter the build workspace, app, source kit or original player's save directory.
    if not plan['memory_cards']:
        yield []; return
    with tempfile.TemporaryDirectory(prefix='muyiunative-route-cards-') as folder:
        for name, data in (plan['card_seed'] or {}).items(): (Path(folder) / name).write_bytes(data)
        yield ['--save-dir', folder]


def run_route(label, binary, command, plan, extra=()):
    with route_cards(plan) as cards: return run(label, [binary, *command, *cards, *extra])


def build_plan(source, profile=None, route=None, frames=None, inputs=None, min_native=None, card_seed=None) -> dict:
    name, image = disc.boot(disc.tracks(source)[0]); serial = disc.serial(name); digest = hashlib.sha256(image).hexdigest()
    candidate = ROOT / 'profiles' / str(serial) / 'game.toml'
    profile = Path(profile).resolve() if profile is not None else candidate if candidate.is_file() else None
    game = tomllib.loads(profile.read_text()) if profile else {}
    if profile and (not isinstance(game.get('disc'), dict) or game['disc'].get('boot_sha256') != digest): raise SystemExit(f'build: the disc\'s boot executable is not the one {profile.name} describes')
    selected = route or ('first_level' if 'first_level' in game else 'validation')
    if route and route not in game: raise SystemExit(f'build: no [{route}] route in the profile')
    v = game.get(selected, {})
    if not isinstance(v, dict): raise SystemExit(f'build: [{selected}] must be a route table')
    cards = v.get('memory_cards', False)
    if type(cards) is not bool: raise SystemExit('build: route memory_cards must be true or false')
    seeded = v.get('requires_card_seed', False)
    if type(seeded) is not bool: raise SystemExit('build: route requires_card_seed must be true or false')
    if seeded and card_seed is None: raise SystemExit(f'build: route {selected} requires --card-seed with initial memory cards')
    policy = game.get('native', {})
    if not isinstance(policy, dict): raise SystemExit('build: [native] must be a table')
    try: required = native_coverage.minimum(min_native if min_native is not None else v.get('min_native_percent', policy.get('min_percent', 100)))
    except ValueError as e: raise SystemExit(f'build: {e}') from e
    count = frames if frames is not None else v.get('frames', 2400)
    if type(count) is not int or count < 1: raise SystemExit('build: --frames must be positive')
    try: events = replay.events(inputs if inputs is not None else v.get('inputs', []), v.get('pulses', []) if inputs is None else (), count)
    except (ValueError, KeyError, TypeError, AttributeError) as e: raise SystemExit(f'build: invalid route: {e}') from e
    title = game.get('name') or serial or Path(source).stem
    if not isinstance(title, str): raise SystemExit('build: the profile name must be a string')
    title = re.sub(r'[/\x00-\x1f:]', '-', title).strip() or 'PS1 Game'
    if len(title.encode()) >= 256: raise SystemExit('build: the game name must be shorter than 256 UTF-8 bytes')
    return dict(serial=serial, boot_sha256=digest, title=title, profile=profile, route=selected, frames=count, inputs=events,
                entry=exe.parse(image).pc, min_native_percent=required,
                memory_cards=cards or card_seed is not None, card_seed=card_seed,
                metal_hashes={scale: v.get(f'metal_{scale}x', {}).get('frame_hash_fnv1a64') for scale in (1, 2, 4)} if frames is None and inputs is None and card_seed is None else {},
                expected_hash=v.get('frame_hash_fnv1a64') if frames is None and inputs is None and card_seed is None else None, save_id=f'{serial or "homebrew"}-{digest}')


def build_routes(source, profile=None, routes=None, frames=None, inputs=None, min_native=None, card_seed=None):
    selected = [routes] if isinstance(routes, str) else routes
    if selected is not None and (not isinstance(selected, list) or not selected or any(not isinstance(r, str) or not r for r in selected) or len(set(selected)) != len(selected)):
        raise SystemExit('build: native routes must be a nonempty list of distinct route names')
    first = build_plan(source, profile, selected[0] if selected else None, frames, inputs, min_native, card_seed)
    if selected is None and first['profile']:
        game = tomllib.loads(first['profile'].read_text()); selected = game.get('native', {}).get('routes')
    if selected is None: return [first]
    if not isinstance(selected, list) or not selected or any(not isinstance(r, str) or not r for r in selected) or len(set(selected)) != len(selected):
        raise SystemExit('build: native routes must be a nonempty list of distinct route names')
    return [build_plan(source, first['profile'], route, frames, inputs, min_native, card_seed) for route in selected]


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


def completed(output, plan):
    match = re.search(r'host: (\d+) frames,', output)
    if not match or int(match[1]) != plan['frames']: raise SystemExit('build: the route exited before its requested frame count')
    result = frame_hash(output)
    if plan['expected_hash'] and result != plan['expected_hash']:
        raise SystemExit(f'build: route {plan["route"]} ends on {result}, not its recorded {plan["expected_hash"]}; inspect the route before changing its profile')
    return result


def make_command(work):
    b = Path(work).relative_to(MAC)
    make = ['make', f'B={b}', f'PYTHON={sys.executable}', '-j3']
    if (ROOT / 'build-kit.json').is_file(): make += ['PREBUILT_METALLIB=../../gfx/metal/ps1gpu.metallib']
    return make


def compile_routes(sources, plans, work, strict=False):
    """Capture every selected route before merging their immutable versions; verify and measure every route against the resulting single binary."""
    work = Path(work); gen = work / 'gen'; gen.mkdir(); make = make_command(work)
    run('building the capture host', [*make, 'host'])
    routes, programs = [], []
    for i, (source, selected) in enumerate(zip(sources, plans), 1):
        hints = analyze.hints_from_profile(selected[0]['profile']) if selected[0]['profile'] else None
        word, entries = codegen.load(source); programs.append((word, analyze.analyze(word, entries, hints)))
        for j, plan in enumerate(selected, 1):
            print(f'build: disc {i}/{len(sources)}; route {plan["route"]}, {plan["frames"]} frames', flush=True)
            stem = gen / f'disc{i}-route{j}'; inputs = stem.with_suffix('.inputs')
            inputs.write_text('\n'.join(plan['inputs']) + ('\n' if plan['inputs'] else ''))
            route = ['--bios', ROOT / 'bios/openbios/openbios.bin', '--disc', source, '--headless', '--frames', plan['frames'], '--input-file', inputs]
            capture = stem.with_suffix('.cap'); captured = run_route('capturing interpreted game code', work / 'muyiunative', route, plan, ['--capture', capture, '--png', stem.with_suffix('.png')])
            expected = completed(captured, plan)
            blocks = overlays.read(Path(f'{capture}.blocks')); summary = overlays.report(blocks)
            word, entries = codegen.load(source, capture=capture); program = analyze.analyze(word, entries, hints); overlays.extend(program, blocks)
            programs.append((word, program))
            routes.append(dict(plan=plan, command=route, expected=expected, coverage=stem.with_suffix('.coverage.json'), disc=i,
                               capture=dict(blocks=summary['blocks'], replaced_addresses=summary['replaced_addresses'])))
    compile_programs(programs, work)
    return routes, verify_captured(routes, work, strict)


def compile_programs(programs, work):
    work = Path(work); gen = work / 'gen'
    for old in gen.glob('game*.c'): old.unlink()   # generated sources owned by this isolated workspace, including obsolete split parts
    print('build: generating native code', flush=True)
    generated = codegen.generate_split(None, codegen.merge(programs), 'game', 20000)
    for i, text in enumerate(generated): (gen / ('game.c' if i == len(generated) - 1 else f'game_{i + 1}.c')).write_text(text)
    parts = ' '.join(str(f.relative_to(MAC)) for f in sorted(gen.glob('game*.c')))
    run('compiling native game code', [*make_command(work), 'game', f'GAME_C={parts}'])


def verify_captured(routes, work, strict=False):
    work = Path(work); evidence = []
    for record in routes:
        plan = record['plan']; print(f'build: verifying disc {record["disc"]}, route {plan["route"]}', flush=True)
        native = run_route('checking the route against the interpreter', work / 'game/muyiunative', record['command'], plan,
                           ['--lockstep-strict' if strict else '--lockstep', '--coverage', record['coverage'], '--coverage-start', f'{plan["entry"]:08x}'])
        if 'the machines agree' not in native or completed(native, plan) != record['expected']:
            raise SystemExit('build: native code differs from the interpreter; no app was packed')
        report = native_coverage.read(record['coverage'])
        try: measured = native_coverage.enforce(report, plan['min_native_percent'])
        except ValueError as e: raise SystemExit(f'build: disc {record["disc"]}, route {plan["route"]}: {e}') from e
        print(f'build: program RAM {measured["percent"]:.6f}% native; {measured["interpreted"]} interpreted instructions', flush=True)
        evidence.append(dict(disc=record['disc'], serial=plan['serial'], boot_sha256=plan['boot_sha256'], route=plan['route'], frames=plan['frames'],
                             frame_hash=record['expected'], coverage=report, acceptance=measured, capture=record['capture']))
        evidence[-1]['memory_cards'] = plan['memory_cards']
        evidence[-1]['card_seed_sha256'] = {name: hashlib.sha256(data).hexdigest() for name, data in (plan['card_seed'] or {}).items()}
    report_path = work / 'native-report.json'
    report_path.write_text(json.dumps(dict(version=1, strict_devices=strict, routes=evidence), indent=2) + '\n')
    return report_path


def check_metal(routes, work, scales):
    """Optional hardware acceptance: GPU-only drawing against a separate software/interpreting console, with zero duplicate CPU raster operations."""
    path = Path(work) / 'native-report.json'; evidence = json.loads(path.read_text()); seen = {}
    for index, record in enumerate(routes):
        plan = record['plan']; disc_number = record['disc']; seen[disc_number] = seen.get(disc_number, 0) + 1
        measured = []
        for scale in dict.fromkeys(scales):
            output = Path(work) / 'gen' / f'disc{disc_number}-route{seen[disc_number]}-metal{scale}.coverage.json'
            # CPU translation has already passed every block. Repeating >1 billion CPU stops at each scale dominated the first long-route GPU check; use periodic
            # CPU/RAM stops here while still comparing every device, VRAM and cache at every frame. The build's preceding per-block CPU acceptance is unchanged.
            result = run_route(f'checking GPU-only Metal {scale}x, disc {disc_number}, route {plan["route"]}', Path(work) / 'game/muyiunative', record['command'], plan,
                               ['--renderer', 'metal', '--scale', scale, '--lockstep', '--lockstep-step', 64, '--coverage', output, '--coverage-start', f'{plan["entry"]:08x}'])
            expected = record['expected'] if scale == 1 else plan['metal_hashes'].get(scale)
            actual = completed(result, dict(plan, expected_hash=expected))
            operations = re.search(r'host: GPU operations: (\d+) software, (\d+) external', result)
            stops = re.search(r'host: lockstep: the machines agree, (\d+) stops', result)
            if not operations or int(operations[1]) != 0 or int(operations[2]) == 0 or not stops:
                raise SystemExit('build: GPU-only verification did not establish independent state agreement and zero duplicate CPU rasterization')
            acceptance = native_coverage.enforce(native_coverage.read(output), plan['min_native_percent'])
            measured.append(dict(scale=scale, frame_hash=actual, software_ops=0, external_ops=int(operations[2]), lockstep_stops=int(stops[1]), cpu_step_cycles=64, acceptance=acceptance))
        evidence['routes'][index]['metal'] = measured
    path.write_text(json.dumps(evidence, indent=2) + '\n')


def build(a):
    sources = disc_set.sources(a.disc, getattr(a, 'extra_disc', None) or ())
    if a.renderer == 'software' and a.scale != 1: raise SystemExit('build: --scale 2 or 4 needs --renderer metal')
    seed = read_card_seed(a.card_seed)
    plans = [build_routes(s, a.profile if i == 0 else None, a.route, a.frames, a.input, a.min_native_percent, seed) for i, s in enumerate(sources)]
    regions = {disc.licence(disc.tracks(s)[0]) for s in sources}
    if len(regions) != 1: raise SystemExit('build: all discs must have the same licence region')
    p = plans[0][0]
    out = (a.output or MAC / 'build/apps' / f'{p["title"]}.app').resolve()
    if out.suffix != '.app': raise SystemExit('build: --output must end in .app')
    bundle_id = f'org.muyiunative.game.{p["save_id"].lower()}'
    if out.exists() and packed_id(out) != bundle_id: raise SystemExit('build: the output exists and is not this game app; choose another --output')
    if out.is_relative_to(ROOT) and not out.is_relative_to(MAC / 'build'): raise SystemExit('build: game apps inside this repository must stay in shell/mac/build')
    # An app can be built outside the checkout, but may not replace its input disc or contain it while pack_app replaces an old bundle.
    if a.disc.resolve().is_relative_to(out) or any(s.is_relative_to(out) or any(t.file.resolve().is_relative_to(out) for t in disc.tracks(s)) for s in sources):
        raise SystemExit('build: the output app contains a source disc or playlist')
    products = MAC / 'build/products'; products.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix=f'{p["serial"] or "homebrew"}-', dir=products)); gen = work / 'gen'
    print(f'build: {p["title"]}; {len(sources)} disc(s)', flush=True)
    print(f'build: local build files in {work}', flush=True)
    records, report_path = compile_routes(sources, plans, work, a.strict_lockstep)
    if a.check_metal: check_metal(records, work, a.check_metal)
    resources = work / 'resources'; resources.mkdir(); catalog = []
    for i, (source, selected) in enumerate(zip(sources, plans), 1):
        plan = selected[0]
        target = 'disc' if i == 1 else f'discs/disc{i:02d}'
        bundle_disc(source, resources / target)
        catalog.append({'Title': f'Disc {i}' + (f' ({plan["serial"]})' if plan['serial'] else ''), 'Disc': f'{target}/disc.cue',
                        'Serial': plan['serial'] or '', 'BootSHA256': plan['boot_sha256']})
    manifest = {'Version': 2 if len(sources) > 1 else 1, 'Title': p['title'], 'Serial': p['serial'] or '', 'BootSHA256': p['boot_sha256'], 'SaveID': p['save_id'], 'Disc': 'disc/disc.cue',
                'Renderer': a.renderer, 'RenderScale': a.scale}
    if len(sources) > 1: manifest['Discs'] = catalog
    (resources / 'game.plist').write_bytes(plistlib.dumps(manifest))
    command = [sys.executable, ROOT / 'tools/pack_app.py', out, '--exe', work / 'game/muyiunative', '--name', p['title'],
        '--bundle-id', bundle_id, '--resource', ROOT / 'bios/openbios', '--resource', resources / 'disc', '--resource', resources / 'game.plist', '--resource', gen / 'ps1gpu.metallib',
        '--resource', report_path]
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
    p.add_argument('--profile', type=Path); p.add_argument('--route', action='append', help='capture and verify this profile route (repeatable; default: native.routes, then first_level/validation)')
    p.add_argument('--min-native-percent', type=float, help='minimum program-RAM native coverage after game entry (default: profile, or 100)')
    p.add_argument('--strict-lockstep', action='store_true', help='also compare every device and VRAM after every block (slow)')
    p.add_argument('--check-metal', type=int, choices=[1, 2, 4], action='append', help='verify GPU-only Metal at this scale before packing (repeatable; needs a Metal device)')
    p.add_argument('--card-seed', type=Path, help='initial card1.mcr/card2.mcr for capture routes; read-only seeds are copied outside the checkout for each run')
    p.add_argument('--renderer', choices=['software', 'metal'], default='software', help='renderer when opening the game app (default: software)')
    p.add_argument('--scale', type=int, choices=[1, 2, 4], default=1, help='Metal internal resolution multiplier (default: 1)')
    p.add_argument('--frames', type=int, help='capture frames (unknown games default to 2400)'); p.add_argument('--input', action='append', help='frame:hex-buttons (repeatable)')
    a = ap.parse_args(argv)
    try: return build(a)
    except (OSError, ValueError, tomllib.TOMLDecodeError) as e: raise SystemExit(f'build: {e}') from e
