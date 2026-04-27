#!/usr/bin/env python3

import os

src = "x86_64-softmmu/qemu-system-x86_64"
link = "bin/remote-stub"

if os.path.lexits(link):
    os.unlink(link)
os.symlink(src, link)