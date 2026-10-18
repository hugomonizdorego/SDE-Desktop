# SPDX-License-Identifier: AGPL-3.0-only
"""Exercise the actual embedded upstream parser through the native executable."""
import math
import subprocess
import sys
binary=sys.argv[1]
for expression, expected, angle in [
    ('2+3*4',14,'deg'), ('(2+3)*4',20,'deg'), ('sin(30)+2^3',8.5,'deg'),
    ('cos(pi)',-1,'rad'), ('sin(100)',1,'grad'), ('sqrt(81)',9,'deg'),
    ('5!',120,'deg'), ('log(1000)',3,'deg'), ('2^10',1024,'deg'),
    ('1/3+2/3',1,'deg'), ('-5+2',-3,'deg')]:
    value=subprocess.check_output([binary,'--eval',expression,'--angle',angle],text=True)
    assert math.isclose(float(value),expected,rel_tol=1e-10,abs_tol=1e-10),(expression,value)
for invalid in ['1/0','sqrt(-1)','hello','2+2;globalThis.bad=1','A'.__class__.__name__ * 600]:
    result=subprocess.run([binary,'--eval',invalid],capture_output=True,text=True)
    assert result.returncode != 0,(invalid,result.stdout)
print('Calculator: precedence, scientific functions, all angle modes and invalid input passed')
