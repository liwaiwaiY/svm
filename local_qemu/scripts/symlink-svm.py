#!/usr/bin/env python3

import json
import os
import shlex
import subprocess
import sys

introspect = os.environ.get('MESONINTROSPECT')
out = subprocess.run([*shlex.split(introspect), '--targets'],
                     stdout=subprocess.PIPE, check=True).stdout
targets = json.loads(out)

src = None
for t in targets:
    if t['name'] == 'qemu-system-x86_64' and t['filename']:
        src = t['filename'][0]
        break

if src is None:
    print("symlink-svm: target 'qemu-system-x86_64' not found, skipping",
          file=sys.stderr)
    sys.exit(0)

link = 'bin/remote-stub'
os.makedirs(os.path.dirname(link), exist_ok=True)
if os.path.lexists(link):
    os.unlink(link)
os.symlink(os.path.relpath(src, os.path.dirname(link)), link)
print(f'symlink-svm: {link} -> {src}')
