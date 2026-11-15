# -*- coding: utf-8 -*-
"""Wrapper: reads a UTF-8 target window title from target_title.txt and runs
scroll_run's main with it, so non-ASCII titles survive shell argv encoding."""
import io
import os
import sys

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import scroll_run

here = os.path.dirname(os.path.abspath(__file__))
title = open(os.path.join(here, "target_title.txt"), encoding="utf-8").read().strip()
sys.argv = ["scroll_run.py", "@" + title] + sys.argv[1:]
scroll_run.main()
