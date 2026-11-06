# python3 bench/sfchart.py: docs/reference/symfony-ceiling{,-dark}.svg from the ./dev sfbench numbers below
X0, X1, MAX = 150, 690, 20000
HELLO = (18182, 16660)  # empty page, APCu / nocopy config
ROWS = [('products?page=3', 17430, 4542), ('product/77', 15801, 4416)]  # nocopy, APCu (no marshaller)
THEMES = {
    '': dict(pri='#1f2328', sec='#59636e', on1='#ffffff', s1='#2a78d6', s2='#eb6834', grid='#e6eaef', axis='#818b98', band='#59636e'),
    '-dark': dict(pri='#f0f6fc', sec='#9198a1', on1='#0d1117', s1='#3987e5', s2='#d95926', grid='#262c36', axis='#656c76', band='#9198a1'),
}
sx = lambda v: X0 + v * (X1 - X0) / MAX
pct = lambda v, h=HELLO[1]: round(100 * v / h)

def bar(y, v, fill):
    w = sx(v) - X0
    return f'<path fill="{fill}" d="M{X0},{y}h{w-4:.1f}a4,4 0 0 1 4,4v8a4,4 0 0 1 -4,4h-{w-4:.1f}z"/>'

for suffix, c in THEMES.items():
    o = []
    for t in range(0, MAX + 1, 5000):
        x = sx(t)
        o.append(f'<line stroke="{c["grid"]}" x1="{x:.1f}" y1="72" x2="{x:.1f}" y2="178"/><text fill="{c["sec"]}" font-size="11" x="{x:.1f}" y="194" text-anchor="middle">{t // 1000}k</text>')
    b0, b1 = sx(HELLO[0]), sx(HELLO[1])
    o.append(f'<rect fill="{c["band"]}" opacity=".35" x="{b0:.1f}" y="66" width="{b1-b0:.1f}" height="112"/>')
    o.append(f'<text fill="{c["sec"]}" x="{b1:.1f}" y="60" text-anchor="end">empty page, no cache: the host\'s limit</text>')
    for i, (name, nv, av) in enumerate(ROWS):
        y = 82 + 56 * i
        o.append(f'<text fill="{c["pri"]}" x="{X0-10}" y="{y+21}" text-anchor="end">{name}</text>')
        o.append(bar(y, nv, c['s1']))
        o.append(f'<text fill="{c["on1"]}" font-weight="600" x="{sx(nv)-8:.1f}" y="{y+12}" text-anchor="end">{nv:,} ({pct(nv)}%)</text>')
        o.append(bar(y + 18, av, c['s2']))
        o.append(f'<text fill="{c["pri"]}" x="{sx(av)+6:.1f}" y="{y+30}">{av:,} ({pct(av, HELLO[0])}%)</text>')
    o.append(f'<line stroke="{c["axis"]}" x1="{X0}" y1="72" x2="{X0}" y2="178"/>')
    desc = '; '.join(f'{n}: nocopy {nv:,} req/s ({pct(nv)}% of an empty page), APCu {av:,} ({pct(av, HELLO[0])}%)' for n, nv, av in ROWS)
    open(f'docs/reference/symfony-ceiling{suffix}.svg', 'w').write(f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 720 232" width="720" height="232" font-family="-apple-system,BlinkMacSystemFont,'Segoe UI',Helvetica,Arial,sans-serif" font-size="12" role="img">
<title>Symfony cache-heavy pages: {desc}. An empty page serves {HELLO[0]:,} to {HELLO[1]:,} req/s.</title>
<text fill="{c["pri"]}" x="0" y="18" font-size="14" font-weight="600">Symfony, cache-heavy pages (req/s, php-fpm, 8 workers)</text>
<rect fill="{c["s1"]}" x="0" y="34" width="12" height="12" rx="2"/><text fill="{c["pri"]}" x="18" y="44">nocopy (bundle)</text>
<rect fill="{c["s2"]}" x="130" y="34" width="12" height="12" rx="2"/><text fill="{c["pri"]}" x="148" y="44">APCu (no marshaller)</text>
{chr(10).join(o)}
<text fill="{c["sec"]}" font-size="11" x="0" y="222">(%) = share of the empty page. Docker on arm64, PHP 8.5, 2026-11-06, ./dev sfbench.</text>
</svg>
''')
