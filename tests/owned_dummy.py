#!/usr/bin/env python3
"""A stand-in for a batch's processes in tests/test_owned_run.py:
    owned_dummy.py tree         start a child that sleeps, and sleep (10 min)
    owned_dummy.py spawn-exit   start a child that sleeps, wait 3 s, exit 0 (leaving it)"""
import subprocess
import sys
import time

child = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(600)"])
if sys.argv[1:] == ["spawn-exit"]:
    time.sleep(3)
    sys.exit(0)
time.sleep(600)
