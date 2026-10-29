"""The repository's own Markdown stays navigable and its entry point short: every relative link and #anchor resolves (GitHub's heading anchors), and AGENTS.md stays at
most 1,500 words (a finding goes into the docs it belongs to, not into it). Needs git, or walks the tree without it; no game files."""
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
