# SPDX-License-Identifier: GPL-3.0-or-later
import os
import subprocess
import sys
import time


def main():
    display = '127.0.0.1:95' if os.environ.get('SDE_XVFB_TCP') else ':95'
    command = ['Xvfb', ':95', '-screen', '0', '800x600x24', '-noreset', '-ac']
    command += ['-listen', 'tcp'] if display.startswith('127') else ['-nolisten', 'tcp']
    env = {**os.environ, 'DISPLAY': display, 'GDK_BACKEND': 'x11'}
    server = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        for _ in range(50):
            if subprocess.run(['xdpyinfo'], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0:
                break
            time.sleep(.1)
        else: raise AssertionError('Xvfb did not start')
        subprocess.run([sys.argv[1]], env=env, check=True)
    finally:
        server.terminate(); server.wait(timeout=5)

if __name__ == "__main__":
    main()
