"""Print printable ASCII strings (>= 6 chars) from a binary that match a regex. Usage: binstrings.py <file> <regex> [max]"""
import re
import sys

path, pattern = sys.argv[1], re.compile(sys.argv[2], re.I)
limit = int(sys.argv[3]) if len(sys.argv) > 3 else 60
data = open(path, "rb").read()
seen = set()
for m in re.finditer(rb"[\x20-\x7e]{6,}", data):
    s = m.group().decode("ascii")
    if pattern.search(s) and s not in seen:
        seen.add(s)
        print(s[:240])
        if len(seen) >= limit:
            break
