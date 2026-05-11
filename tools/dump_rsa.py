#!/usr/bin/env python3
"""Convert an openssl-dumped RSA private key + signature into a C header.

Usage:
    openssl rsa -in test.key -text -noout > /tmp/keydump.txt
    python3 tools/dump_rsa.py <keydump.txt> <sig-file> <out.h>
"""
import re, sys

if len(sys.argv) != 4:
    sys.exit("usage: dump_rsa.py <keydump.txt> <sig-file> <out.h>")

dump_path, sig_path, out_path = sys.argv[1:]
out = open(dump_path).read()
sig = open(sig_path, 'rb').read()

def grab(name):
    m = re.search(rf'{name}:\s*\n((?:\s+[0-9a-f:]+\n)+)', out)
    if not m: sys.exit(f'no {name}')
    return bytes.fromhex(re.sub(r'[^0-9a-f]', '', m.group(1)))

def trim(b):
    while len(b) > 1 and b[0] == 0: b = b[1:]
    return b

mod   = trim(grab('modulus'))
prive = trim(grab('privateExponent'))
p     = trim(grab('prime1'))
q     = trim(grab('prime2'))

with open(out_path, 'w') as f:
    def emit(name, b):
        f.write(f'static const uint8_t {name}[] = {{\n')
        for i in range(0, len(b), 12):
            f.write('    ' + ', '.join(f'0x{x:02x}' for x in b[i:i+12]) + ',\n')
        f.write('};\n')
        f.write(f'#define {name.upper()}_LEN {len(b)}\n\n')
    emit('rsa_n',   mod)
    emit('rsa_d',   prive)
    emit('rsa_p',   p)
    emit('rsa_q',   q)
    emit('rsa_sig', sig)
print(f'wrote {out_path}')
