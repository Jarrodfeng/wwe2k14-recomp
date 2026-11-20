# fixture: invented python with a triple-quoted string whose lines sit at column 0 (review finding 10)


def has_doc(x):
    t = """
column zero text inside the string
"""
    y = x + len(t)
    return y


def next_one():
    return 0
