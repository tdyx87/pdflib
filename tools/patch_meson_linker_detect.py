import re

p = "F:/conan/p/mesonc1b5314f43342/f/bin/mesonbuild/linkers/detect.py"
s = open(p, encoding="utf-8").read()

NL = chr(10)
pat = re.compile(r"(?m)^([ \t]*)(\w+), o, (\w+) = Popen_safe(\w*)\((.*?)\)[ \t]*$")

count = [0]


def repl2(m):
    indent, lhs, errname, fn, args = m.groups()
    count[0] += 1
    lines = [
        "%s%s, o, %s = Popen_safe%s(%s)" % (indent, lhs, errname, fn, args),
        "%sif o is None:" % indent,
        "%s    o = ''" % indent,
        "%sif %s is None:" % (indent, errname),
        "%s    %s = ''" % (indent, errname),
    ]
    return NL.join(lines)


s2 = pat.sub(repl2, s)
open(p, "w", encoding="utf-8").write(s2)
print("patched:", count[0])
