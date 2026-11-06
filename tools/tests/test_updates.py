"""Launcher update packaging and published-feed selection use synthetic files/metadata; no network, Keychain, game data or GPU is needed."""
import argparse, base64, hashlib, io, sys, tarfile, tempfile, unittest
import xml.etree.ElementTree as ET
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import sparkle
import update_feed as feed
import pack_app


class Updates(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup); self.root = Path(self.temp.name)
        self.zip = self.root / 'preview.zip'; self.zip.write_bytes(b'synthetic launcher archive')
        self.xml = self.root / 'muyiunative-0.9.0-mac-arm64.appcast.xml'; self.signature = base64.b64encode(bytes(64)).decode()
        feed.create(self.zip, self.xml, '0.9.0', '100', '14.0', self.signature)
        self.release = {'tag_name': 'v0.9.0', 'prerelease': True, 'draft': False, 'published_at': '2026-11-06T12:00:00Z', 'assets': [
            {'name': self.zip.name, 'state': 'uploaded', 'size': self.zip.stat().st_size},
            {'name': self.xml.name, 'state': 'uploaded', 'browser_download_url': feed.WEB + '/releases/download/v0.9.0/' + self.xml.name}]}

    def test_preview_feed_keeps_signed_bytes_and_matches_published_asset(self):
        data = self.xml.read_bytes() + b'<!-- signature must be preserved verbatim -->\n'
        self.assertEqual(feed.published_feed([self.release], lambda url: data), data)
        self.assertEqual(feed.candidate(self.release, data), 100)
        tree = ET.fromstring(data); self.assertIn('rebuild', tree.findtext('./channel/item/description', ''))

    def test_highest_build_wins_without_drafts_or_unsigned_old_releases(self):
        latest = self.xml.read_bytes(); old = latest.replace(b'>100<', b'>90<')
        older = dict(self.release, assets=[dict(a) for a in self.release['assets']]); older['assets'][1]['name'] = 'older.appcast.xml'
        older['assets'][1]['browser_download_url'] = feed.WEB + '/releases/download/v0.9.0/older.appcast.xml'
        draft = dict(self.release, draft=True)
        unsigned = dict(self.release, assets=[])
        self.assertEqual(feed.published_feed([draft, older, unsigned, self.release], lambda url: old if url.endswith('older.appcast.xml') else latest), latest)
        self.assertEqual(feed.published_feed([unsigned]), (feed.ROOT / 'updates/empty-appcast.xml').read_bytes())

    def test_wrong_tag_remote_download_or_archive_size_is_rejected(self):
        original = self.xml.read_bytes()
        for data in [original.replace(b'>0.9.0<', b'>0.8.0<'), original.replace(b'https://github.com/', b'https://example.com/'), original.replace(b'length="26"', b'length="1"')]:
            # The exact synthetic file size is part of the contract, independent of the fixture's spelling.
            if data == original: data = original.replace(f'length="{self.zip.stat().st_size}"'.encode(), b'length="1"')
            with self.assertRaises(ValueError): feed.candidate(self.release, data)
        self.release['assets'][1]['browser_download_url'] = 'https://example.com/feed.xml'
        with self.assertRaisesRegex(ValueError, 'outside'): feed.published_feed([self.release], lambda url: original)

    def test_malformed_or_unsigned_archive_metadata_never_advertises_update(self):
        original = self.xml.read_bytes()
        for data in [b'not XML', original.replace(self.signature.encode(), b''), b'<!DOCTYPE rss>' + original]:
            with self.assertRaises((ValueError, ET.ParseError)): feed.candidate(self.release, data)

    def test_launcher_only_configuration_requires_signed_feed_and_archive(self):
        a = argparse.Namespace(name='muyiunative', bundle_id=None, version=None, sparkle=self.root)
        with patch.object(pack_app, 'metadata', lambda: ('0.9.0', '100', 'abc')):
            info = pack_app.info_plist(a, '14.0')
            self.assertEqual(info['SUPublicEDKey'], sparkle.CONFIG['public_key'])
            self.assertTrue(info['SURequireSignedFeed']); self.assertTrue(info['SUVerifyUpdateBeforeExtraction'])
            self.assertFalse(info['SUAutomaticallyUpdate']); self.assertFalse(info['SUEnableSystemProfiling'])
            a.sparkle = None; self.assertNotIn('SUFeedURL', pack_app.info_plist(a, '14.0'))
        a = argparse.Namespace(out=self.root / 'Game.app', exe=self.zip, name='Game', bundle_id='org.muyiunative.game.example', sparkle=self.root)
        with self.assertRaisesRegex(SystemExit, 'only the generic launcher'): pack_app.pack(a)

    def test_framework_copy_preserves_links_and_signs_inside_out_without_xpc(self):
        source = self.root / 'Sparkle.framework'; contents = self.root / 'Contents'; contents.mkdir()
        (source / 'Versions/B/XPCServices').mkdir(parents=True)
        (source / 'Versions/Current').symlink_to('B'); (source / 'XPCServices').symlink_to('Versions/Current/XPCServices')
        (source / 'Sparkle').symlink_to('Versions/Current/Sparkle')
        (source / 'Versions/B/Sparkle').write_bytes(b'synthetic library')
        calls = []; sparkle.embed(source, contents, '-', calls.append)
        target = contents / 'Frameworks/Sparkle.framework'
        self.assertTrue((target / 'Sparkle').is_symlink()); self.assertFalse((target / 'XPCServices').exists())
        self.assertTrue((source / 'Versions/B/XPCServices').exists(), 'the cached official framework remains unchanged')
        self.assertEqual([str(Path(c[-1]).relative_to(target)) for c in calls], sparkle.FRAMEWORK_CODE)
        calls.clear(); sparkle.sign(target, 'Developer ID', calls.append, timestamp=True)
        self.assertTrue(all('--timestamp' in c and '--deep' not in c for c in calls))

    def test_dependency_checksum_is_checked_before_extraction(self):
        archive = io.BytesIO()
        with tarfile.open(fileobj=archive, mode='w:xz') as t:
            item = tarfile.TarInfo('Sparkle.framework/Versions/B/Sparkle'); item.size = 4; t.addfile(item, io.BytesIO(b'fake'))
        data = archive.getvalue(); config = dict(sparkle.CONFIG, sha256='0' * 64)
        with patch.object(sparkle, 'CONFIG', config), patch.object(sparkle.urllib.request, 'urlopen', lambda *a, **k: io.BytesIO(data)):
            with self.assertRaisesRegex(ValueError, 'checksum'): sparkle.fetch(self.root / 'cached')
        self.assertFalse((self.root / 'cached').exists())
        config['sha256'] = hashlib.sha256(data).hexdigest()
        with patch.object(sparkle, 'CONFIG', config), patch.object(sparkle.urllib.request, 'urlopen', lambda *a, **k: io.BytesIO(data)):
            self.assertEqual((sparkle.fetch(self.root / 'cached') / 'Sparkle.framework/Versions/B/Sparkle').read_bytes(), b'fake')


if __name__ == '__main__': unittest.main()
