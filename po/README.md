# Translating SDE

SDE uses GNU gettext. All user-visible text in the source code is written in
US English and marked with `_("...")`.

1. Refresh the template: `cmake --build build --target pot` (writes `po/sde.pot`).
2. Start a language: `msginit -i po/sde.pot -o po/<code>.po -l <code>`
   (for example `id` for Indonesian).
3. Translate the `msgstr` entries, add `<code>` to `po/LINGUAS` and rebuild.
4. Update an existing translation after the template changed:
   `msgmerge -U po/<code>.po po/sde.pot`.

Compiled catalogs are installed to `/usr/share/locale/<code>/LC_MESSAGES/sde.mo`.
