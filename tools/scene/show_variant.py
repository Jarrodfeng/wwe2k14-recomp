#!/usr/bin/env python3
"""Prints the inputs and named constants of effect variants from src\\renderer_scene\\fx_tables.h (a reading aid).
  py -I tools\\scene\\show_variant.py yBG_Default_base_vertex [more names or prefixes]"""
import os
import re
import sys

HEADER = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'src', 'renderer_scene', 'fx_tables.h')
text = open(HEADER, encoding='utf-8').read()
inputs = dict((m.group(1), m.group(2)) for m in re.finditer(r'inline constexpr Input kI(\d+)\[\] = \{(.*?)\};', text))
fields = dict((m.group(1), m.group(2)) for m in re.finditer(r'inline constexpr Field kF(\d+)\[\] = \{(.*?)\};', text))
for m in re.finditer(r'\{"(\w+)", (nullptr|kI(\d+)), \d+, (nullptr|kF(\d+)), \d+\}', text):
    name = m.group(1)
    if not any(name == a or name.startswith(a) for a in sys.argv[1:]):
        continue
    print(name)
    if m.group(3):
        print('  in :', re.sub(r'\{(\d+), (\d), "(\w+)"\}', r'\1:\3/\2', inputs[m.group(3)]))
    if m.group(5):
        print('  c  :', re.sub(r'\{"(\w+)", (\d+), (\d+)\}', r'\1@\2x\3', fields[m.group(5)]))
