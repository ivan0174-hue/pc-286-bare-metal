import json, sys, os, subprocess, random
src, dst, per = sys.argv[1], sys.argv[2], int(sys.argv[3])
os.makedirs(dst, exist_ok=True)
order = ['ax','bx','cx','dx','cs','ss','ds','es','sp','bp','si','di','ip','flags']
for fn in sorted(os.listdir(src)):
    if not fn.endswith('.MOO.gz'): continue
    base = fn[:-7]
    tmpj = '/tmp/_t.json'
    subprocess.run(['python3', os.environ.get('MOO2JSON', 'moo2json.py'), os.path.join(src, fn), tmpj], capture_output=True)
    tests = json.load(open(tmpj))
    random.seed(1)
    if len(tests) > per: tests = random.sample(tests, per)
    with open(os.path.join(dst, base + '.txt'), 'w') as f:
        for t in tests:
            ir = t['initial']['regs']
            fr = dict(ir); fr.update(t['final']['regs'])
            exc = t.get('exception', {}).get('number', -1)
            fa = t.get('exception', {}).get('flag_address', -1)
            f.write('%s|%s|%s|%s|%s|%s|%d %d\n' % (
                t['name'], ' '.join('%02x' % b for b in t['bytes']),
                ' '.join(str(ir[k]) for k in order),
                ' '.join('%d:%d' % (a, v) for a, v in t['initial']['ram']),
                ' '.join(str(fr[k]) for k in order),
                ' '.join('%d:%d' % (a, v) for a, v in t['final']['ram']),
                exc, fa))
    print(base, len(tests), flush=True)
