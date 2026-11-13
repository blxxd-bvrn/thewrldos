#!/usr/bin/env python3
"""A small Shader Model 4 assembler: turns shader assembly in the syntax the HLSL
compiler's disassembler prints (vs_4_0 / ps_4_0, dcl_*, dp4 o0.x, v0.xyzw,
cb0[0].xyzw ...) into DXBC bytecode that Direct3D 10's CreateVertexShader /
CreatePixelShader accept, with ISGN/OSGN signatures and the container checksum.

THE WRLD OS has no HLSL compiler (Microsoft's fxc runs on Windows only); this
is how samples/d3d10cube.c gets its shaders:

    python3 tools/dxbc_asm.py samples/d3d10cube.sm4 samples/d3d10cube_shaders.h

The input holds one or more shaders, each started by a line
    shader NAME
followed by signature lines
    in  SEMANTIC INDEX REGISTER MASK [sysvalue]    (e.g.  in POSITION 0 0 xyz)
    out SEMANTIC INDEX REGISTER MASK [sysvalue]    (e.g.  out SV_Position 0 0 xyzw position)
and then the program, starting with vs_4_0 or ps_4_0. ';' starts a comment.
"""
import re, struct, sys

OPS = {
    'add': 0, 'and': 1, 'break': 2, 'breakc_z': 3, 'breakc_nz': 3, 'continue': 7, 'continuec_z': 8, 'continuec_nz': 8,
    'deriv_rtx': 11, 'deriv_rty': 12, 'discard_z': 13, 'discard_nz': 13, 'div': 14, 'dp2': 15, 'dp3': 16, 'dp4': 17,
    'else': 18, 'endif': 21, 'endloop': 22, 'eq': 24, 'exp': 25, 'frc': 26, 'ftoi': 27, 'ftou': 28, 'ge': 29,
    'iadd': 30, 'if_z': 31, 'if_nz': 31, 'ieq': 32, 'ige': 33, 'ilt': 34, 'imad': 35, 'imax': 36, 'imin': 37,
    'imul': 38, 'ine': 39, 'ineg': 40, 'ishl': 41, 'ishr': 42, 'itof': 43, 'ld': 45, 'log': 47, 'loop': 48,
    'lt': 49, 'mad': 50, 'min': 51, 'max': 52, 'mov': 54, 'movc': 55, 'mul': 56, 'ne': 57, 'nop': 58, 'not': 59,
    'or': 60, 'resinfo': 61, 'ret': 62, 'retc_z': 63, 'retc_nz': 63, 'round_ne': 64, 'round_ni': 65, 'round_pi': 66,
    'round_z': 67, 'rsq': 68, 'sample': 69, 'sample_l': 72, 'sample_b': 74, 'sqrt': 75, 'sincos': 77, 'udiv': 78,
    'ult': 79, 'uge': 80, 'umul': 81, 'umad': 82, 'umax': 83, 'umin': 84, 'ushr': 85, 'utof': 86, 'xor': 87,
}
CONDITIONAL = {'breakc_z', 'breakc_nz', 'continuec_z', 'continuec_nz', 'discard_z', 'discard_nz', 'if_z', 'if_nz', 'retc_z', 'retc_nz'}
SYSVAL = {'': 0, 'position': 1, 'vertex_id': 6, 'instance_id': 8, 'is_front_face': 9, 'target': 0}
COMP = 'xyzw'


def f2u(x):
    return struct.unpack('<I', struct.pack('<f', float(x)))[0]


def operand(text, dst=False, select1=False):
    """Encode one operand: list of dwords."""
    t = text.strip()
    mod = 0
    if t.startswith('-'):
        mod |= 1; t = t[1:].strip()
    if t.startswith('|') and '|' in t[1:]:
        mod |= 2; t = t[1:t.rindex('|')] + t[t.rindex('|') + 1:]
    m = re.fullmatch(r'l\((.*)\)', t)
    if m:                                                        # immediate
        vals = [v.strip() for v in m.group(1).split(',')]
        words = []
        for v in vals:
            # 0x... is a raw 32-bit value (integers, masks); anything else is a float
            words.append(int(v, 16) & 0xFFFFFFFF if v.lower().startswith('0x') else f2u(v.rstrip('f')))
        nc = 1 if len(words) == 1 else 2
        while nc == 2 and len(words) < 4:
            words.append(0)
        return [(nc) | (4 << 12)] + words
    m = re.fullmatch(r'(cb|icb|r|v|o|t|s|x)(\d+)?((?:\[[^\]]+\])*)(?:\.([xyzw]{1,4}))?', t)
    if not m:
        raise SystemExit('cannot parse operand: ' + text)
    kind, num, idx, comps = m.group(1), m.group(2), m.group(3), m.group(4)
    types = {'r': 0, 'v': 1, 'o': 2, 'x': 3, 's': 6, 't': 7, 'cb': 8, 'icb': 9}
    ty = types[kind]
    indices = []
    if num is not None:
        indices.append(int(num))
    indices += [int(i) for i in re.findall(r'\[(\d+)\]', idx)]
    if re.search(r'\[[^\]]*[a-z]', idx):
        raise SystemExit('relative indexing is not supported by this assembler: ' + text)
    tok = (ty << 12) | (len(indices) << 20)
    if ty == 6:                                                  # samplers have no components
        pass
    elif dst:
        mask = 0
        for c in (comps or 'xyzw'):
            mask |= 1 << COMP.index(c)
        tok |= 2 | (0 << 2) | (mask << 4)
    elif select1:
        c = (comps or 'x')[0]
        tok |= 2 | (2 << 2) | (COMP.index(c) << 4)
    else:
        sw = comps or 'xyzw'
        sw = (sw + sw[-1] * 4)[:4]
        tok |= 2 | (1 << 2)
        for i, c in enumerate(sw):
            tok |= COMP.index(c) << (4 + 2 * i)
    words = []
    if mod:
        tok |= 0x80000000
        words.append(1 | (mod << 6))
    return [tok] + words + indices


def split_operands(s):
    out, depth, cur = [], 0, ''
    for ch in s:
        if ch in '([':
            depth += 1
        elif ch in ')]':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur); cur = ''
        else:
            cur += ch
    if cur.strip():
        out.append(cur)
    return [o.strip() for o in out]


def mask_dst(text):
    """dcl operands: a register with a component mask"""
    return operand(text, dst=True)


def assemble(lines):
    kind, toks = None, []
    for raw in lines:
        line = raw.split(';')[0].strip()
        if not line:
            continue
        if re.fullmatch(r'(vs|ps)_4_[01]', line):
            kind = 1 if line.startswith('vs') else 0
            minor = int(line[-1])
            version = (kind << 16) | (4 << 4) | minor
            continue
        word, _, rest = line.partition(' ')
        rest = rest.strip()
        sat = word.endswith('_sat')
        if sat:
            word = word[:-4]
        if word == 'dcl_temps':
            toks.append([104 | (2 << 24), int(rest)])
        elif word == 'dcl_constantbuffer':
            m = re.match(r'cb(\d+)\[(\d+)\]', rest)
            toks.append([89, 0x00208E46, int(m.group(1)), int(m.group(2))])
        elif word == 'dcl_sampler':
            m = re.match(r's(\d+)', rest)
            toks.append([90, 0x00106000, int(m.group(1))])
        elif word == 'dcl_resource_texture2d':
            m = re.search(r't(\d+)', rest)
            toks.append([88 | (3 << 11), 0x00107000, int(m.group(1)), 0x5555])
        elif word in ('dcl_input', 'dcl_output'):
            toks.append([(95 if word == 'dcl_input' else 101)] + mask_dst(rest))
        elif word in ('dcl_input_ps', 'dcl_input_ps_siv', 'dcl_output_siv', 'dcl_input_siv'):
            parts = [p.strip() for p in rest.split(',')]
            interp = 2                                           # linear (perspective-correct)
            reg = parts[0]
            if ' ' in reg:
                mode, reg = reg.split()
                interp = {'constant': 1, 'linear': 2, 'linear_noperspective': 4}.get(mode, 2)
            op = {'dcl_input_ps': 98, 'dcl_input_ps_siv': 100, 'dcl_output_siv': 103, 'dcl_input_siv': 97}[word]
            t = [op | ((interp << 11) if word.startswith('dcl_input_ps') else 0)] + mask_dst(reg)
            if word != 'dcl_input_ps':
                t.append(SYSVAL[parts[1]])
            toks.append(t)
        elif word in OPS:
            op = OPS[word]
            tok = op | (0x2000 if sat else 0) | (0x40000 if word.endswith('_nz') else 0)
            ops = split_operands(rest) if rest else []
            words = []
            for i, o in enumerate(ops):
                cond = word in CONDITIONAL
                if word in ('sincos', 'imul', 'umul', 'udiv'):
                    is_dst = i < 2
                else:
                    is_dst = i == 0 and not cond
                if o == 'null':
                    words += [0x0000D000]
                    continue
                words += operand(o, dst=is_dst, select1=cond)
            toks.append([tok] + words)
        else:
            raise SystemExit('unknown instruction: ' + line)
    body = []
    for t in toks:
        if (t[0] & 0x7FF) != 53:
            t[0] = (t[0] & ~(0x7F << 24)) | (len(t) << 24)
        body += t
    return [version, len(body) + 2] + body


def signature(elems, tag):
    """elems: (name, index, register, mask, sysval)"""
    head = struct.pack('<II', len(elems), 8)
    names, table = b'', b''
    name_base = 8 + 24 * len(elems)
    offsets = {}
    for name, *_ in elems:
        if name not in offsets:
            offsets[name] = name_base + len(names)
            names += name.encode() + b'\0'
    while len(names) % 4:
        names += b'\xab'
    for name, index, reg, mask, sysval in elems:
        rw = mask if tag == b'ISGN' else 0
        table += struct.pack('<IIIIIBBBB', offsets[name], index, sysval, 3, reg, mask, rw, 0, 0)
    return head + table + names


# MD5 with the DXBC container's own final padding (as in vkd3d and Wine)
def _md5_transform(state, block):
    import math
    K = [int(abs(math.sin(i + 1)) * 2 ** 32) & 0xFFFFFFFF for i in range(64)]
    S = [7, 12, 17, 22] * 4 + [5, 9, 14, 20] * 4 + [4, 11, 16, 23] * 4 + [6, 10, 15, 21] * 4
    X = struct.unpack('<16I', block)
    a, b, c, d = state
    rol = lambda x, n: ((x << n) | (x >> (32 - n))) & 0xFFFFFFFF
    for i in range(64):
        if i < 16: f, g = (b & c) | (~b & d), i
        elif i < 32: f, g = (d & b) | (~d & c), (5 * i + 1) % 16
        elif i < 48: f, g = b ^ c ^ d, (3 * i + 5) % 16
        else: f, g = c ^ (b | (~d & 0xFFFFFFFF)), (7 * i) % 16
        f = (f + a + K[i] + X[g]) & 0xFFFFFFFF
        a, d, c, b = d, c, b, (b + rol(f, S[i])) & 0xFFFFFFFF
    return [(x + y) & 0xFFFFFFFF for x, y in zip(state, (a, b, c, d))]


def dxbc_checksum(data):
    state = [0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476]
    full = len(data) // 64 * 64
    for i in range(0, full, 64):
        state = _md5_transform(state, data[i:i + 64])
    rest = bytearray(data[full:])
    bits = len(data) * 8
    count = len(rest)
    rest += b'\x80'
    count += 1
    padding = 64 - count
    if padding < 8:
        rest += b'\0' * padding
        state = _md5_transform(state, bytes(rest))
        block = bytearray(64)
    else:
        block = bytearray(4) + rest + bytearray(padding - 4)
    struct.pack_into('<I', block, 0, bits & 0xFFFFFFFF)
    struct.pack_into('<I', block, 60, ((bits >> 2) | 1) & 0xFFFFFFFF)
    state = _md5_transform(state, bytes(block))
    return struct.pack('<4I', *state)


def container(isgn, osgn, prog):
    chunks = [(b'ISGN', isgn), (b'OSGN', osgn), (b'SHDR', struct.pack('<%dI' % len(prog), *prog))]
    head = 32 + 4 * len(chunks)
    offs, blob = [], b''
    for tag, data in chunks:
        offs.append(head + len(blob))
        blob += tag + struct.pack('<I', len(data)) + data
    total = head + len(blob)
    rest = struct.pack('<III', 1, total, len(chunks)) + struct.pack('<%dI' % len(offs), *offs) + blob
    return b'DXBC' + dxbc_checksum(rest) + rest


def main():
    src, dst = sys.argv[1], sys.argv[2]
    shaders, cur = [], None
    for line in open(src):
        s = line.split(';')[0].strip()
        if s.startswith('shader '):
            cur = {'name': s.split()[1], 'in': [], 'out': [], 'code': []}
            shaders.append(cur)
        elif s.startswith(('in ', 'out ')) and cur is not None and not cur['code']:
            p = s.split()
            mask = 0
            for ch in p[4]:
                mask |= 1 << COMP.index(ch)
            cur['in' if p[0] == 'in' else 'out'].append((p[1], int(p[2]), int(p[3]), mask, SYSVAL.get(p[5] if len(p) > 5 else '', 0)))
        elif cur is not None:
            cur['code'].append(line)
    out = ['/* generated by tools/dxbc_asm.py from %s - do not edit */' % src.split('/')[-1]]
    for sh in shaders:
        blob = container(signature(sh['in'], b'ISGN'), signature(sh['out'], b'OSGN'), assemble(sh['code']))
        out.append('static const unsigned char %s[%d] = {' % (sh['name'], len(blob)))
        for i in range(0, len(blob), 16):
            out.append('    ' + ', '.join('0x%02x' % b for b in blob[i:i + 16]) + ',')
        out.append('};')
    open(dst, 'w').write('\n'.join(out) + '\n')


if __name__ == '__main__':
    main()
