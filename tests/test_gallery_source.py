# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the prepared fork's identity, original legal text and native integration."""
import hashlib
import json
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
source = Path(sys.argv[1]) if len(sys.argv) > 1 else root/'vendor/gallery'
record = json.loads((root/'suite/gallery.lock.json').read_text())
versions = json.loads((root/'suite/versions.json').read_text())
assert record['version'] == '4.0'
assert record['sha256'] == 'a24aaa72b5637f49097dedc9c141a33615da770d029315f3285ebf2156cf7b61'
config = (source/'src/meson.build').read_text()
assert "'APP_NAME', 'Gallery'" in config
assert "'APP_ID', 'org.debion.Gallery'" in config
assert "exec_name = 'gallery'" in config
assert versions['gallery'] in config
app = (source/'src/Application.vala').read_text()
for name in ['DebION Dev-team', 'Paolo Bacchilega', 'about.add_link', 'Adw.StyleManager']:
    assert name in app, name
assert 'Copyright' in (source/'COPYING').read_text()
assert 'Paolo Bacchilega' in (source/'data/appdata/org.debion.Gallery.metainfo.xml.in').read_text()
assert 'domain="gallery"' in (source/'data/schemas/org.debion.Gallery.gschema.xml').read_text()
assert (source/'data/gallery.1').is_file()
assert not (source/'data/gthumb.1').exists()
assert 'gallery.desktop</launchable>' in (source/'data/appdata/org.debion.Gallery.metainfo.xml.in').read_text()
assert (source/'src/Types.vala').read_text().startswith('const string APP_DIR = "sde-gallery"')
assert '"--wallpaper"' in (source/'src/Ext/DesktopBackground/DesktopBackground.vala').read_text()
assert 'org.debion.Gallery' in (source/'data/schemas/org.debion.Gallery.gschema.xml').read_text()
archive = root/'source-work/downloads/gthumb-4.0.tar.xz'
if archive.exists(): assert hashlib.sha256(archive.read_bytes()).hexdigest() == record['sha256']
print('Gallery source identity, original notices, About, theme, wallpaper and checksum passed.')
