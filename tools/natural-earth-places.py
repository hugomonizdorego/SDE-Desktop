#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Regenerates data/gallery/places.tsv, the offline list of places that the
Gallery's Places plugin names photo locations with:

  tools/natural-earth-places.py ne_10m_populated_places_simple.geojson \\
      > data/gallery/places.tsv

The input is Natural Earth's 1:10m "populated places (simple)" layer as
GeoJSON (public domain, https://www.naturalearthdata.com/; the GeoJSON copy
is in https://github.com/nvkelso/natural-earth-vector, geojson/). Each line
of the output is

  name <TAB> region <TAB> country <TAB> latitude <TAB> longitude <TAB> population

sorted by latitude, with the positions rounded to about 100 m. The region
is only kept where a country has two places of the same name ("Portland,
Oregon" and "Portland, Maine"), and a few country names are shortened the
way people write them in a caption ("United States", "Hong Kong"), which
keeps the file small. Nothing is downloaded: the Gallery never needs a
network to name a place."""
import collections
import json
import sys

COUNTRY = {
    'United States of America': 'United States',
    'Congo (Kinshasa)': 'DR Congo',
    'Congo (Brazzaville)': 'Republic of the Congo',
    'Hong Kong S.A.R.': 'Hong Kong',
    'Macau S.A.R': 'Macau',
    'Guinea Bissau': 'Guinea-Bissau',
    'The Bahamas': 'Bahamas',
    'The Gambia': 'Gambia',
}


def clean(text):
    return ' '.join(str(text or '').replace('\t', ' ').split())


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    with open(sys.argv[1], encoding='utf-8') as f:
        data = json.load(f)
    rows = []
    for feature in data.get('features', []):
        props = feature.get('properties') or {}
        geometry = feature.get('geometry') or {}
        coords = geometry.get('coordinates') or [props.get('longitude'), props.get('latitude')]
        name = clean(props.get('name') or props.get('nameascii'))
        if not name or coords[0] is None or coords[1] is None:
            continue
        country = clean(props.get('adm0name') or props.get('sov0name'))
        country = COUNTRY.get(country, country)
        region = clean(props.get('adm1name'))
        if region in (name, country):
            region = ''
        population = int(props.get('pop_max') or props.get('pop_min') or 0)
        rows.append((round(float(coords[1]), 3), round(float(coords[0]), 3), name, region, country, max(population, 0)))
    rows.sort()
    names = collections.Counter((r[2], r[4]) for r in rows)
    rows = [r if names[(r[2], r[4])] > 1 else r[:3] + ('',) + r[4:] for r in rows]
    out = sys.stdout
    out.write('# Places of the world for the Gallery\'s Places plugin (tools/natural-earth-places.py).\n')
    out.write('# Made with Natural Earth, public domain: https://www.naturalearthdata.com/\n')
    out.write('# name\tregion\tcountry\tlatitude\tlongitude\tpopulation\n')
    for lat, lon, name, region, country, population in rows:
        out.write(f'{name}\t{region}\t{country}\t{lat:.3f}\t{lon:.3f}\t{population}\n')


if __name__ == '__main__':
    main()
