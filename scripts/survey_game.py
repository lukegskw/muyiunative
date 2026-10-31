#!/usr/bin/env python3
# Survey an unknown game on the Mac (plan.md, phase 4). No profile is needed: boot an external disc, apply the given inputs, and capture each code version that ran.
# Report only hashes, addresses and counts, and write a draft profile for the observed route. The capture itself contains game data and stays under shell/mac/build.
# This is observation, not a compatibility claim: inspect the last frame locally before naming the draft route's screen. Usage: scripts/survey_game.py <disc.cue> ...
import argparse, hashlib, json, sys
from pathlib import Path

from native_game import MAC, ROOT, frame_hash, step
sys.path.insert(0, str(ROOT / 'tools'))
import disc, overlays, replay


def main():
    p = argparse.ArgumentParser(description='Survey code loaded by an unknown game and draft its profile; all outputs stay in shell/mac/build')
    p.add_argument('disc', type=Path)
    p.add_argument('--frames', type=int, default=2400)
    p.add_argument('--input', action='append', default=[], help='<frame>:<hex-buttons>, repeated in frame order')
    a = p.parse_args()
    if a.frames < 1: p.error('--frames must be positive')
    name, exe = disc.boot(disc.tracks(a.disc)[0]); serial = disc.serial(name)
    if not serial: raise SystemExit('survey_game: the boot executable has no serial for a profile')
    out = MAC / 'build' / 'survey' / serial; out.mkdir(parents=True, exist_ok=True)
    r, _ = step('building the survey host', ['make', '-C', MAC], part='survey_game')
    if r.returncode: raise SystemExit('survey_game: the host does not build')
    try: inputs = replay.events(a.input, frames=a.frames)
    except ValueError as e: raise SystemExit(f'survey_game: invalid input: {e}') from e
    (out / 'route.inputs').write_text('\n'.join(inputs) + ('\n' if inputs else ''))
    route = ['--disc', a.disc, '--headless', '--frames', a.frames, '--input-file', out / 'route.inputs']
    r, seconds = step(f'surveying {serial}, {a.frames} frames', [MAC / 'build' / 'ps1native', *route, '--capture', out / 'capture', '--png', out / 'last.png'], part='survey_game')
    h = frame_hash(r)
    if r.returncode or not h: raise SystemExit('survey_game: the route did not finish')
    report = overlays.report(overlays.read(out / 'capture.blocks'))
    report.update(serial=serial, boot_sha256=hashlib.sha256(exe).hexdigest(), frames=a.frames, inputs=a.input, frame_hash_fnv1a64=h)
    (out / 'survey.json').write_text(json.dumps(report, indent=2) + '\n')
    (out / 'game.toml').write_text(f'# Draft from scripts/survey_game.py; inspect last.png locally before naming the screen.\nserial = "{serial}"\n'
                                f'\n[disc]\nboot_sha256 = "{report["boot_sha256"]}"\n\n[validation]\nscreen = "unverified survey"\n'
                                f'frames = {a.frames}\ninputs = {json.dumps(a.input)}\nframe_hash_fnv1a64 = "{h}"\n')
    print(f'survey_game: {serial}: {report["blocks"]} code versions, {report["replaced_addresses"]} addresses with multiple versions, {seconds:.1f} s; '
          f'report, draft profile and local picture in {out.relative_to(ROOT)}')
    return 0


if __name__ == '__main__': sys.exit(main())
