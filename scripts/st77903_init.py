"""st77903_init.py <objdump -dr модуля> — последовательность инициализации ST77903 из st77903_8bit_power_on:
вызовы m_spi_write(слово) — бит 8: 1 — данные, 0 — команда; задержки — msleep/usleep_range. Печатает
«команда: данные…» по строке на команду."""
import re
import sys

lines = open(sys.argv[1], encoding='utf-8', errors='replace').read().split('\n')
start = next(i for i, l in enumerate(lines) if '<st77903_8bit_power_on>:' in l)
body = []
for l in lines[start + 1:]:
    if re.match(r'^[0-9a-f]+ <', l):
        break
    body.append(l)

ins = []   # (текст инструкции, цель вызова или None)
fn = {}    # регистр → функция, адрес которой в нём (lui/addiu с перемещением LO16); .text+0 — m_spi_write
for l in body:
    m = re.match(r'\s+([0-9a-f]+):\s+[0-9a-f]{8}\s+(.*)', l)
    if m:
        ins.append([m.group(2).strip(), None])
        continue
    m = re.match(r'\s+[0-9a-f]+: R_MIPS_26\s+(\S+)', l)
    if m and ins:
        ins[-1][1] = m.group(1)
    m = re.match(r'\s+[0-9a-f]+: R_MIPS_LO16\s+(\S+)', l)
    if m and ins:
        r = re.match(r'addiu\s+(\w+),', ins[-1][0])
        if r:
            fn[r.group(1)] = 'm_spi_write' if m.group(1) == '.text' else m.group(1)
            ins[-1][0] = 'fnset ' + r.group(1) + ' ' + fn[r.group(1)]


def val(op, regs):
    m = re.match(r'li\s+(\w+),(-?(?:0x)?[0-9a-f]+)', op)
    if m:
        return m.group(1), int(m.group(2), 0)
    m = re.match(r'(?:addiu|ori)\s+(\w+),zero,(-?(?:0x)?[0-9a-f]+)', op)
    if m:
        return m.group(1), int(m.group(2), 0)
    m = re.match(r'move\s+(\w+),zero', op)
    if m:
        return m.group(1), 0
    m = re.match(r'move\s+(\w+),(\w+)', op)
    if m and m.group(2) in regs:
        return m.group(1), regs[m.group(2)]
    return None


regs = {}
seq = []
i = 0
while i < len(ins):
    op, target = ins[i]
    if op.startswith('fnset'):
        _, r, name = op.split()
        fn[r] = name
        i += 1
        continue
    if op.startswith('jalr'):
        target = fn.get(op.split()[1], 'jalr')
    if target:
        # слот задержки выполняется до вызова
        if i + 1 < len(ins):
            v = val(ins[i + 1][0], regs)
            if v:
                regs[v[0]] = v[1]
        name = target or 'jalr'
        if name == 'm_spi_write':
            seq.append(('w', regs.get('a0')))
        elif 'sleep' in name or 'delay' in name:
            seq.append(('d', name, regs.get('a0'), regs.get('a1')))
        elif name != 'jalr':
            seq.append(('call', name))
        i += 2
        continue
    v = val(op, regs)
    if v:
        regs[v[0]] = v[1]
    i += 1

out = []
for s in seq:
    if s[0] == 'w':
        w = s[1]
        if w is None:
            out.append(['?'])
        elif w & 0x100:
            if out and out[-1][0] not in ('delay', '?'):
                out[-1].append('%02X' % (w & 0xff))
            else:
                out.append(['data', '%02X' % (w & 0xff)])
        else:
            out.append(['%02X' % w])
    elif s[0] == 'd':
        out.append(['delay', s[1], str(s[2]), str(s[3])])
    else:
        out.append(['call', s[1]])
for o in out:
    print(' '.join(o))
