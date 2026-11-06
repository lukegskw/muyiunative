#!/usr/bin/env python3
# A release ships its signed appcast beside the ZIP. The publishing workflow copies the newest published feed verbatim, preserving its EdDSA signature.
import argparse, base64, json, os, re, urllib.parse, urllib.request
import xml.etree.ElementTree as ET
from email.utils import formatdate
from pathlib import Path

REPO = 'lukegskw/muyiunative'
WEB = f'https://github.com/{REPO}'
NS = 'http://www.andymatuschak.org/xml-namespaces/sparkle'
ET.register_namespace('sparkle', NS)
ROOT = Path(__file__).resolve().parents[1]


def create(archive, output, version, build, minos, signature):
    if not re.fullmatch(r'\d+\.\d+\.\d+', version) or not build.isdecimal(): raise ValueError('invalid release version')
    if len(base64.b64decode(signature, validate=True)) != 64: raise ValueError('invalid EdDSA archive signature')
    feed = ET.Element('rss', version='2.0'); channel = ET.SubElement(feed, 'channel')
    ET.SubElement(channel, 'title').text = 'muyiunative updates'
    ET.SubElement(channel, 'link').text = WEB + '/releases'
    item = ET.SubElement(channel, 'item'); ET.SubElement(item, 'title').text = f'muyiunative {version}'
    ET.SubElement(item, 'pubDate').text = formatdate(usegmt=True)
    ET.SubElement(item, 'link').text = WEB + f'/releases/tag/v{version}'
    for name, value in [('version', build), ('shortVersionString', version), ('minimumSystemVersion', minos)]: ET.SubElement(item, f'{{{NS}}}{name}').text = value
    ET.SubElement(item, 'description').text = 'After updating the launcher, rebuild your game apps with Build Game App to update them. Existing game apps keep their current version until rebuilt.'
    ET.SubElement(item, 'enclosure', {'url': WEB + f'/releases/download/v{version}/' + urllib.parse.quote(archive.name),
                  'length': str(archive.stat().st_size), 'type': 'application/octet-stream', f'{{{NS}}}edSignature': signature})
    ET.indent(feed); Path(output).write_bytes(ET.tostring(feed, encoding='utf-8', xml_declaration=True) + b'\n')


def get(url):
    headers = {'User-Agent': 'muyiunative-update-feed', 'Accept': 'application/vnd.github+json'}
    if urllib.parse.urlsplit(url).netloc == 'api.github.com' and os.environ.get('GH_TOKEN'): headers['Authorization'] = 'Bearer ' + os.environ['GH_TOKEN']
    with urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=30) as response:
        data = response.read(1024 * 1024 + 1)
    if len(data) > 1024 * 1024: raise ValueError('update metadata exceeds 1 MiB')
    return data


def candidate(release, data):
    if b'<!DOCTYPE' in data.upper(): raise ValueError('unexpected appcast document type')
    feed = ET.fromstring(data); items = feed.findall('./channel/item')
    if feed.tag != 'rss' or len(items) != 1: raise ValueError('each release must provide exactly one update')
    item = items[0]; build = item.findtext(f'{{{NS}}}version', '')
    version = item.findtext(f'{{{NS}}}shortVersionString', '')
    if not re.fullmatch(r'\d{1,12}', build) or release['tag_name'] != 'v' + version: raise ValueError('release/appcast version mismatch')
    enclosure = item.find('enclosure')
    if enclosure is None or len(base64.b64decode(enclosure.get(f'{{{NS}}}edSignature', ''), validate=True)) != 64: raise ValueError('missing archive signature')
    prefix = WEB + '/releases/download/' + urllib.parse.quote(release['tag_name'], safe='') + '/'
    archives = [a for a in release['assets'] if a['name'].endswith('.zip') and a.get('state') == 'uploaded']
    if len(archives) != 1 or enclosure.get('url') != prefix + urllib.parse.quote(archives[0]['name']) or enclosure.get('length') != str(archives[0]['size']):
        raise ValueError('appcast does not describe the published launcher asset')
    return int(build)


def published_feed(releases, download=get):
    candidates = []
    for release in releases:
        if release.get('draft') or not release.get('published_at'): continue
        # Previews are supported: GitHub's /releases/latest would hide the project's current prereleases.
        assets = [a for a in release['assets'] if a['name'].endswith('.appcast.xml') and a.get('state') == 'uploaded']
        if not assets: continue   # releases predating automatic updates have no signed feed
        if len(assets) != 1: raise ValueError('ambiguous appcast assets')
        asset = assets[0]; expected = WEB + '/releases/download/' + urllib.parse.quote(release['tag_name'], safe='') + '/' + urllib.parse.quote(asset['name'])
        if asset['browser_download_url'] != expected: raise ValueError('appcast asset is outside the release')
        data = download(expected); candidates.append((candidate(release, data), data))
    return max(candidates, key=lambda pair: pair[0])[1] if candidates else (ROOT / 'updates/empty-appcast.xml').read_bytes()


def refresh(output):
    releases = []
    for page in range(1, 101):
        batch = json.loads(get(f'https://api.github.com/repos/{REPO}/releases?per_page=100&page={page}'))
        if not isinstance(batch, list): raise ValueError('invalid release response')
        releases += batch
        if len(batch) < 100: break
    else: raise ValueError('too many release pages')
    Path(output).write_bytes(published_feed(releases))


if __name__ == '__main__':
    p = argparse.ArgumentParser(description='Select the signed appcast from published GitHub releases, including previews')
    p.add_argument('output', type=Path)
    try: refresh(p.parse_args().output)
    except (OSError, ValueError, KeyError, ET.ParseError) as error: raise SystemExit(f'updates: {error}') from error
