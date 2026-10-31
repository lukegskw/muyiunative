# Deterministic input routes for capture and lockstep (plan.md, phase 4). Base masks are held until changed; a pulse adds buttons for `on` frames every `period`,
# from `start` up to `stop`. Overlapping pulses combine by OR. This represents repeated fire/jump presses without thousands of lines in a game's profile.
# The resulting ordered frame:hex-mask file is read once by the Mac host; it changes no machine timing and is shared by all validation runs.
from functools import reduce
from operator import or_


def events(inputs, pulses=(), frames=None):
    changes, last = {}, -1
    for s in inputs:
        parts = s.split(':')
        if len(parts) != 2: raise ValueError('an input must be frame:hex-buttons')
        frame, buttons = int(parts[0]), int(parts[1], 16)
        if frame < last or frame < 0 or not 0 <= buttons <= 0xffff: raise ValueError('inputs must be ordered, nonnegative frames and 16-bit masks')
        last = frame
        if frames is None or frame < frames: changes.setdefault(frame, []).append((-1, buttons))
    for i, p in enumerate(pulses):
        start, stop, period, on, buttons = (p[k] for k in ('start', 'stop', 'period', 'on', 'buttons'))
        if not all(isinstance(v, int) for v in (start, stop, period, on, buttons)) or not (0 <= start < stop and 0 < on < period and 0 < buttons <= 0xffff):
            raise ValueError('a pulse needs 0 <= start < stop, 0 < on < period, and a 16-bit button mask')
        stop = min(stop, frames) if frames is not None else stop
        for frame in range(start, stop, period):
            changes.setdefault(frame, []).append((i, buttons))
            changes.setdefault(min(frame + on, stop), []).append((i, 0))
    base, active, previous, out = 0, {}, 0, []
    for frame, changes_here in sorted(changes.items()):
        for which, buttons in changes_here:
            if which == -1: base = buttons
            else: active[which] = buttons
        buttons = reduce(or_, active.values(), base)
        if buttons != previous: out.append(f'{frame}:{buttons:x}'); previous = buttons
    return out
