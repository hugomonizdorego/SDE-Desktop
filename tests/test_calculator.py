# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Selviana DE Project and contributors
"""Exercise the native Calculator executable without a display: --eval,
programmer --base, --convert, --version and invalid input."""
import json
import subprocess
import sys
from pathlib import Path

binary = sys.argv[1]
root = Path(__file__).resolve().parents[1]


def run(*args):
    return subprocess.run([binary, *args], capture_output=True, text=True, timeout=20)


for expression, expected, angle in [
        ('2 + 2', '4', 'deg'), ('2+3*4', '14', 'deg'), ('(2+3)*4', '20', 'deg'),
        ('sin(30)+2^3', '8.5', 'deg'), ('cos(pi)', '-1', 'rad'), ('sin(100)', '1', 'grad'),
        ('sqrt(81)', '9', 'deg'), ('5!', '120', 'deg'), ('log(1000)', '3', 'deg'),
        ('2^10', '1024', 'deg'), ('1/3+2/3', '1', 'deg'), ('-5+2', '-3', 'deg'),
        ('0.1+0.2', '0.3', 'deg'), ('2^3^2', '512', 'deg'), ('200+10%', '220', 'deg'),
        ('2π', '6.283185307179586', 'deg'), ('2(3+4)', '14', 'deg')]:
    result = run('--eval', expression, '--angle', angle)
    assert result.returncode == 0 and result.stdout.strip() == expected, (expression, result)

for invalid in ['1/0', 'sqrt(-1)', 'hello', '2+2;globalThis.bad=1', 'str' * 600, '2+']:
    result = run('--eval', invalid)
    assert result.returncode == 2 and result.stdout == '' and result.stderr.strip(), (invalid, result)
assert run('--eval', '1/0').stderr.strip() == 'Cannot divide by zero'

for expression, base, bits, expected in [('FF + 1', 'hex', '64', '100'), ('12 AND 10', 'dec', '64', '8'),
                                         ('127 + 1', 'dec', '8', '-128'), ('1010 << 10', 'bin', '64', '101000')]:
    result = run('--eval', expression, '--base', base, '--bits', bits)
    assert result.returncode == 0 and result.stdout.strip() == expected, (expression, result)

for value, src, dst, expected in [('1', 'km', 'm', '1000'), ('100', 'c', 'f', '212'), ('1', 'in', 'cm', '2.54')]:
    result = run('--convert', value, src, dst)
    assert result.returncode == 0 and result.stdout.strip() == expected, (value, src, dst, result)
assert run('--convert', '1', 'km', 'kg').returncode == 2

version = json.loads((root / 'suite/versions.json').read_text())['calculator']
output = run('--version').stdout
assert version in output and 'Selviana DE Project' in output and 'NO WARRANTY' in output, output
assert run('--bogus').returncode == 2
print('Calculator: --eval, programmer, converter, errors and version passed')
