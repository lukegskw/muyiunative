"""The repository's Markdown stays navigable and AGENTS.md stays at most 1,500 words. Required CI checks run on every PR, and compatibility reports identify the build,
disc revision and verified scope. Needs git for tracked Markdown, or walks the tree without it; no game files or YAML dependency."""
import re, subprocess, unittest
from pathlib import Path
from urllib.parse import unquote

REPO = Path(__file__).resolve().parents[2]
AGENTS_WORDS = 1500


def md_files():   # tracked and new (not ignored) Markdown files
    try:
        out = subprocess.run(['git', 'ls-files', '-co', '--exclude-standard', '*.md'], cwd=REPO, capture_output=True, text=True, check=True).stdout.splitlines()
        return sorted(p for p in (REPO / f for f in out) if p.is_file())
    except (OSError, subprocess.CalledProcessError):
        return sorted(p for p in REPO.rglob('*.md') if '.git' not in p.relative_to(REPO).parts)


def slug(heading):   # GitHub's anchor for a heading: lower case, punctuation dropped, spaces as hyphens
    return re.sub(r'[^\w\- ]', '', heading.strip().lower()).replace(' ', '-')


def anchors(path):   # a repeated heading gets -1, -2, …
    seen, out, fence = {}, set(), False
    for line in path.read_text().splitlines():
        if line.startswith('```'): fence = not fence
        m = None if fence else re.match(r'#{1,6}\s+(.*?)\s*#*$', line)
        if not m: continue
        s = slug(m.group(1)); n = seen.get(s, 0); seen[s] = n + 1
        out.add(s if n == 0 else f'{s}-{n}')
    return out


def links(path):   # (line number, target) of each inline link outside code
    text = re.sub(r'```.*?```', lambda m: '\n' * m.group(0).count('\n'), path.read_text(), flags=re.S)
    for n, line in enumerate(text.splitlines(), 1):
        for m in re.finditer(r'\]\(([^)\s]+)\)', re.sub(r'`[^`]*`', '', line)): yield n, m.group(1)


class Docs(unittest.TestCase):
    def test_required_checks_report_on_every_pr(self):
        # GitHub leaves a required workflow pending when a path/branch filter skips the PR. A docs-only change must still produce all four checks.
        checks = set()
        for name in ('tests.yml', 'lint-python.yml'):
            workflow = (REPO / '.github/workflows' / name).read_text()
            triggers = re.search(r'^on:\n(.*?)(?=^[^\s#])', workflow, re.M | re.S)
            self.assertIsNotNone(triggers, name)
            assert triggers is not None
            events = triggers.group(1)
            self.assertRegex(events, r'(?m)^  pull_request:\s*\n')
            self.assertNotRegex(events, r'(?m)^\s+(?:paths(?:-ignore)?|branches-ignore):')
            pr = re.search(r'^  pull_request:(.*?)(?=^  [a-z]|\Z)', events, re.M | re.S)
            assert pr is not None
            self.assertNotRegex(pr.group(1), r'(?m)^\s+\S+:')
            checks.update(re.findall(r'^    name: (.+)$', workflow, re.M))
        self.assertEqual(checks, {'Mac app', 'Runtime and Mac host tests (plain, then ASan and UBSan)', 'Tool tests', 'ruff and pyright'})

    def test_compatibility_report_captures_revision_and_scope(self):
        # Contract checks for the dependency-free form: GitHub validates the YAML schema. Revision identity and graded results keep "boots" distinct from "finished".
        form = (REPO / '.github/ISSUE_TEMPLATE/compatibility_report.yml').read_text()
        fields = {}
        for block in re.split(r'^  - type: ', form, flags=re.M)[1:]:
            if block.startswith('markdown\n'): continue
            field = re.search(r'^    id: ([\w-]+)$', block, re.M)
            self.assertIsNotNone(field)
            assert field is not None
            key = field.group(1)
            self.assertNotIn(key, fields)
            fields[key] = block
        required = {'game', 'serial', 'region', 'boot-sha256', 'app-version', 'device', 'result', 'renderer', 'controller', 'notes'}
        self.assertTrue(required <= fields.keys())
        for key in required:
            self.assertIn('    validations:\n      required: true', fields[key], key)
        result = fields['result']
        self.assertTrue(result.startswith('dropdown\n'))
        options = re.findall(r'^        - (.+)$', result, re.M)
        self.assertEqual(options[0], 'Does not build')
        self.assertEqual(options[-1], 'Finished')
        self.assertGreater(len(options), 2)
        self.assertNotIn('      default:', result)
        self.assertIn('BootSHA256', fields['boot-sha256'])
        self.assertIn('unavailable', fields['boot-sha256'])
        for forbidden in ('disc image', 'BIOS dump', 'game app', 'generated code', 'memory card', 'save state', 'extracted game data'):
            self.assertIn(forbidden, fields['no-game-data'])
        self.assertIn('          required: true', fields['no-game-data'])

    def test_relative_links_and_anchors_resolve(self):
        bad = []
        for p in md_files():
            for n, t in links(p):
                if re.match(r'[a-z][a-z0-9+.-]*:', t, re.I): continue   # https:, mailto:
                file, _, anchor = t.partition('#')
                dest = (p.parent / unquote(file)).resolve() if file else p
                where = f'{p.relative_to(REPO)}:{n}: {t}'
                if not dest.exists(): bad.append(f'{where} (no such file)')
                elif anchor and dest.suffix == '.md' and unquote(anchor).lower() not in anchors(dest): bad.append(f'{where} (no such heading)')
        self.assertEqual(bad, [], '\n'.join(bad))

    def test_agents_md_stays_short(self):
        words = len((REPO / 'AGENTS.md').read_text().split())
        self.assertLessEqual(words, AGENTS_WORDS, f'AGENTS.md has {words} words: move the details into the docs it points to')


if __name__ == '__main__':
    unittest.main()
