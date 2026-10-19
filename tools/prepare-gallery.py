#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Verify the GNOME release archive and prepare the complete SDE Gallery fork."""
import argparse
import hashlib
import json
import re
import shutil
import tarfile
import tempfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def patch(source, app, version):
    # Keep upstream source headers, license documents and source URLs intact.
    # Resource paths, schemas and application ID are private to SDE Gallery.
    for path in source.rglob('*'):
        if not path.is_file() or path.is_symlink() or path.suffix not in {'.vala', '.xml', '.in', '.build', '.ui'}:
            continue
        text = path.read_text()
        text = text.replace('org.gnome.gthumb', 'org.debion.Gallery').replace('/org/gnome/gthumb', '/org/debion/Gallery')
        text = text.replace('About Thumbnails', 'About Gallery').replace('domain="gthumb"', 'domain="gallery"')
        path.write_text(text)
    for path in sorted(source.rglob('*'), key=lambda p: len(p.parts), reverse=True):
        if 'org.gnome.gthumb' in path.name:
            path.rename(path.with_name(path.name.replace('org.gnome.gthumb', 'org.debion.Gallery')))
    def edit(name, old, new):
        path = source / name
        text = path.read_text()
        if old not in text:
            raise ValueError('Upstream Gallery recipe no longer matches: ' + name)
        path.write_text(text.replace(old, new))
    edit('meson.build', "project('gthumb'", "project('gallery'")
    edit('src/meson.build', "config.set_quoted('APP_NAME', 'Thumbnails')", "config.set_quoted('APP_NAME', 'Gallery')")
    edit('src/meson.build', 'app_version = meson.project_version()', "app_version = '" + version + "'")
    edit('src/meson.build', "exec_name = 'gthumb'", "exec_name = 'gallery'")
    # Translations use Gallery's private gettext domain. Internal MIME types
    # and serialized catalog formats remain readable by the upstream engine.
    edit('data/meson.build', "install_man('gthumb.1')", "install_man('gallery.1')")
    manual = source/'data/gthumb.1'
    text = manual.read_text().replace('gthumb', 'gallery').replace('Thumbnails', 'Gallery').replace('GNOME Applications', 'SDE Applications')
    (source/'data/gallery.1').write_text(text + '\nSDE Gallery integration: Hugo Moniz do Rego <https://github.com/hugomonizdorego>.\n')
    manual.unlink()
    edit('src/Types.vala', 'APP_DIR = "gthumb"', 'APP_DIR = "sde-gallery"')
    edit('src/Application.vala', '"Paolo Bacchilega <paobac@src.gnome.org>",',
         '"Paolo Bacchilega <paobac@src.gnome.org>",\n\t\t\t\t"Hugo Moniz do Rego https://github.com/hugomonizdorego — SDE Gallery",')
    edit('src/Application.vala', 'Adw.show_about_dialog (active_window,', 'var about = new Adw.AboutDialog ();\n\t\t\tabout.set (')
    edit('src/Application.vala', '"website", "https://gitlab.gnome.org/GNOME/gthumb/",',
         '"website", "https://github.com/hugomonizdorego/SDE-Desktop",\n\t\t\t\t"developer-name", "Hugo Moniz do Rego · SDE contributors",\n\t\t\t\t"copyright", "Copyright © Paolo Bacchilega and gThumb contributors; SDE adaptations © 2026 Hugo Moniz do Rego",')
    edit('src/Application.vala', '"issue-url", "https://gitlab.gnome.org/GNOME/gthumb/-/issues",',
         '"issue-url", "https://github.com/hugomonizdorego/SDE-Desktop/issues",')
    edit('src/Application.vala', '"developers", developers\n\t\t\t);',
         '"developers", developers\n\t\t\t);\n\t\t\tabout.add_link ("gthumb", "https://gitlab.gnome.org/GNOME/gthumb");\n\t\t\tabout.add_link ("Hugo Moniz do Rego", "https://github.com/hugomonizdorego");\n\t\t\tabout.present (active_window);')
    # libadwaita controls its own appearance: read the same SDE preferences,
    # without relying on GTK_THEME (unsupported by libadwaita).
    theme = (ROOT / 'suite/gallery-theme.vala.inc').read_text()
    edit('src/Application.vala', 'base.startup ();', 'base.startup ();\n' + theme)
    # --version must work headlessly before GStreamer or GTK initializes.
    edit('src/Main.vala', 'Intl.bindtextdomain', 'if (args.length == 2 && args[1] == "--version") {\n\t\tstdout.printf ("Gallery ' + version + ' (gThumb 4.0)\\n");\n\t\treturn 0;\n\t}\n\tIntl.bindtextdomain')
    # The GNOME portal wallpaper backend does not control the SDE desktop.
    (source/'src/Ext/DesktopBackground/DesktopBackground.vala').write_text((ROOT/'suite/gallery-background.vala').read_text())
    path = source/'data/appdata/org.debion.Gallery.metainfo.xml.in'
    text = path.read_text()
    text = re.sub(r'  <replaces>[\s\S]*?</replaces>\n', '', text)
    text = re.sub(r'  <provides>[\s\S]*?</provides>\n', '', text)
    text = text.replace('<translation type="gettext">gthumb</translation>', '<translation type="gettext">gallery</translation>')
    text = text.replace('<name>Thumbnails</name>', '<name>Gallery</name>').replace('org.debion.Gallery.desktop</launchable>', 'gallery.desktop</launchable>')
    text = text.replace('<developer id="org.gnome">\n    <name translate="no">Paolo Bacchilega</name>', '<developer id="org.debion">\n    <name translate="no">Hugo Moniz do Rego and SDE contributors</name>')
    text = text.replace('<description>', '<description>\n    <p>SDE Gallery, based on gThumb 4.0 by Paolo Bacchilega and contributors. Original copyright and GPL notices are retained.</p>', 1)
    path.write_text(text)
    notice = ('Gallery ' + version + ' — SDE native fork\nSDE developer: Hugo Moniz do Rego\n'
              'Developer: https://github.com/hugomonizdorego\nSDE source: https://github.com/hugomonizdorego/SDE-Desktop\n'
              'Original developer: Paolo Bacchilega and gThumb contributors\n'
              'Upstream: ' + app['url'] + '\nArchive: ' + app['archive'] + '\nSHA256: ' + app['sha256'] + '\n'
              'License: GPL-2.0-or-later (individual files retain their notices)\n'
              'SDE changes: application ID, paths, Gallery branding, About, theme, wallpaper and Debian integration.\n')
    (source/'SDE-UPSTREAM-NOTICE').write_text(notice)
    (source/'copyright').write_text(notice + '\n' + (source/'COPYING').read_text())


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, default=ROOT/'vendor/gallery')
    p.add_argument('--cache', type=Path, default=ROOT/'source-work/downloads')
    args = p.parse_args()
    lock = ROOT/'suite/gallery.lock.json'
    version_file = ROOT/'suite/versions.json'
    app = json.loads(lock.read_text())
    inputs = [lock, version_file, Path(__file__), ROOT/'suite/gallery-theme.vala.inc', ROOT/'suite/gallery-background.vala']
    signature = hashlib.sha256(b''.join(x.read_bytes() for x in inputs)).hexdigest()
    marker = args.output/'.sde-gallery-lock'
    if marker.exists() and marker.read_text().strip() == signature:
        print('SDE Gallery sources already prepared'); return
    if args.output.exists():
        p.error('Preserve existing Gallery sources and choose another --output directory')
    args.cache.mkdir(parents=True, exist_ok=True)
    archive = args.cache/Path(app['archive']).name
    if not archive.exists():
        temporary = archive.with_suffix('.download')
        urllib.request.urlretrieve(app['archive'], temporary)
        temporary.rename(archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != app['sha256']:
        raise ValueError('Gallery upstream SHA256 mismatch: ' + str(archive))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=args.output.parent, prefix='gallery-') as temporary:
        with tarfile.open(archive) as tar:
            tar.extractall(temporary, filter='data')
        source = Path(temporary)/'gthumb-4.0'
        patch(source, app, json.loads(version_file.read_text())['gallery'])
        (source/'.sde-gallery-lock').write_text(signature+'\n')
        source.rename(args.output)
    print('Prepared Gallery from verified gThumb 4.0', flush=True)

if __name__ == '__main__': main()
