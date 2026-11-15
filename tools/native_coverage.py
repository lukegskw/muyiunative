"""Validate native execution reports and enforce exact per-route program-RAM coverage. Reports contain addresses and counters, never game instruction bytes."""
import json, math, re
from fractions import Fraction
from pathlib import Path


def minimum(value: object = 100) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not 0 <= value <= 100 or not math.isfinite(value):
        raise ValueError('minimum native percentage must be a finite number from 0 to 100')
    return float(value)


def address(value): return isinstance(value, str) and re.fullmatch(r'[0-9a-f]{8}', value) is not None


def read(path):
    report = json.loads(Path(path).read_text())
    if not isinstance(report, dict) or type(report.get('version')) is not int or report['version'] != 1 or type(report.get('started')) is not bool or not address(report.get('entry')):
        raise ValueError('invalid native coverage header')
    regions = report.get('regions')
    if not isinstance(regions, dict) or set(regions) != {'program_ram', 'kernel_ram', 'bios_rom', 'other'}: raise ValueError('invalid native coverage regions')
    for counts in regions.values():
        if not isinstance(counts, dict) or set(counts) != {'native', 'interpreted'} or any(type(v) is not int or v < 0 for v in counts.values()):
            raise ValueError('invalid native instruction counts')
    fallbacks = report.get('fallbacks')
    if not isinstance(fallbacks, list): raise ValueError('invalid fallback report')
    for f in fallbacks:
        if (not isinstance(f, dict) or not address(f.get('pc')) or f.get('region') not in regions or
                f.get('reason') not in {'no_entry', 'code_changed', 'delay_slot', 'disabled'} or type(f.get('hits')) is not int or f['hits'] <= 0):
            raise ValueError('invalid fallback record')
    if sum(f['hits'] for f in fallbacks) != sum(v['interpreted'] for v in regions.values()): raise ValueError('fallback hits do not match interpreted instructions')
    return report


def enforce(report, required: float = 100):
    required = minimum(required)
    if not report['started']: raise ValueError('the game executable entry was never reached')
    counts = report['regions']['program_ram']; native, interpreted = counts['native'], counts['interpreted']; total = native + interpreted
    if not total: raise ValueError('the route executed no program-RAM instructions')
    ratio = Fraction(str(required))
    if native * 100 * ratio.denominator < ratio.numerator * total:
        raise ValueError(f'program RAM is {100 * native / total:.6f}% native ({interpreted} interpreted instructions); requires {required}%; inspect the fallback report')
    return dict(native=native, interpreted=interpreted, percent=100 * native / total, minimum_percent=required)
