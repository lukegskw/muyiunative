#!/usr/bin/env python3
# The GUI's exact build entry point (plan.md, phase 7). Copy the signed kit to a unique cache workspace; CLT Python/clang build there, never in the app or a checkout.
# A separate process group owns every make/compiler/capture child. Cancellation stops that group before deleting the workspace, on failure as well as success.
import argparse, os, shutil, signal, subprocess, sys, tempfile, time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.dont_write_bytecode = True
if sys.version_info < (3, 9): raise SystemExit('[RED] build: update Apple Command Line Tools; Python 3.9 or newer is required')
from build_kit import manifest
from disc_set import sources

ROOT = Path(__file__).resolve().parents[1]


class Cancelled(Exception): pass
_cancelled = False


def cancel(signum, frame):
    global _cancelled
    _cancelled = True


def stop_group(process):
    try: os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError: pass
    # The leader can exit before the packer has removed its staging bundle. Allow the whole group to unwind, then stop any child ignoring TERM.
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        process.poll()
        try: os.killpg(process.pid, 0)
        except ProcessLookupError: break
        time.sleep(0.05)
    try: os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError: pass
    process.wait()


def build(args, kit=ROOT):
    global _cancelled
    _cancelled = False; process = None; work = None
    previous = {s: signal.signal(s, cancel) for s in (signal.SIGTERM, signal.SIGINT)}
    try:
        kit = Path(kit).resolve(); manifest(kit)
        source, out, cache = args.disc.resolve(), args.output.resolve(), args.work_root.resolve()
        extra = getattr(args, 'extra_disc', None) or []
        sources(source, extra)   # validate the complete set before allocating a build workspace
        if out.suffix != '.app': raise ValueError('the output must end in .app')
        if out.is_relative_to(kit) or out.is_relative_to(cache): raise ValueError('the output app must be outside the kit and temporary build cache')
        if cache.is_relative_to(out): raise ValueError('the output app must not contain the build cache')
        cache.mkdir(parents=True, exist_ok=True); work = Path(tempfile.mkdtemp(prefix='build-', dir=cache))
        print('build: preparing the launcher build kit', flush=True)
        copy = work / 'kit'; shutil.copytree(kit, copy, ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
        env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', PYTHONNOUSERSITE='1')
        env.pop('PYTHONPATH', None); env.pop('PYTHONHOME', None)
        command = [sys.executable, '-B', copy / 'scripts/muyiunative', 'build', source, '--output', out]
        for disc in extra: command += ['--disc', disc.resolve()]
        if args.frames is not None: command += ['--frames', str(args.frames)]
        if args.profile is not None: command += ['--profile', args.profile.resolve()]
        if _cancelled: raise Cancelled()
        process = subprocess.Popen([str(x) for x in command], cwd=work, env=env, start_new_session=True)
        while True:
            if _cancelled: raise Cancelled()
            try: result = process.wait(timeout=0.1); break
            except subprocess.TimeoutExpired: pass
        if result: raise ValueError(f'the game build failed ({result}); see the build log')
        print(f'build: completed: {out}', flush=True); return 0
    except Cancelled:
        # Ignore another Cancel/quit while stopping children and cleaning the workspace.
        for s in previous: signal.signal(s, signal.SIG_IGN)
        if process is not None:
            stop_group(process); process = None   # cancellation test: signalling the already reaped group again in finally returned EPERM
        print('build: cancelled; temporary files removed', flush=True); return 130
    finally:
        if process is not None: stop_group(process)
        if work is not None: shutil.rmtree(work)
        for s, handler in previous.items(): signal.signal(s, handler)


def main():
    ap = argparse.ArgumentParser(description='Build a native game app using the launcher kit and Apple Command Line Tools')
    ap.add_argument('disc', type=Path); ap.add_argument('--disc', dest='extra_disc', type=Path, action='append')
    ap.add_argument('--output', type=Path, required=True); ap.add_argument('--work-root', type=Path, required=True)
    ap.add_argument('--frames', type=int); ap.add_argument('--profile', type=Path)
    args = ap.parse_args()
    try: return build(args)
    except (OSError, ValueError) as e: print(f'[RED] build: {e}', file=sys.stderr, flush=True); return 1


if __name__ == '__main__': sys.exit(main())
