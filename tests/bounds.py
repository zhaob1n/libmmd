#!/usr/bin/env python3
"""Bounds check: every drawn coordinate (shapes, path points incl. control points, text anchors) of each
SVG must lie inside its root viewBox (tolerance TOL user units). Usage: bounds.py file.svg..."""
import re, sys, xml.etree.ElementTree as ET
TOL = 1.0
NUM = r'[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?'
def nums(s): return [float(x) for x in re.findall(NUM, s or '')]
def tf(t):
    """affine (a,b,c,d,e,f) for translate/scale/matrix/rotate chains"""
    import math
    M = (1, 0, 0, 1, 0, 0)
    for name, args in re.findall(r'(\w+)\s*\(([^)]*)\)', t or ''):
        a = nums(args)
        if name == 'translate': N = (1, 0, 0, 1, a[0], a[1] if len(a) > 1 else 0)
        elif name == 'scale': N = (a[0], 0, 0, a[1] if len(a) > 1 else a[0], 0, 0)
        elif name == 'matrix': N = tuple(a[:6])
        elif name == 'rotate':
            r = math.radians(a[0]); c, s = math.cos(r), math.sin(r)
            cx, cy = (a[1], a[2]) if len(a) > 2 else (0, 0)
            N = (c, s, -s, c, cx - c * cx + s * cy, cy - s * cx - c * cy)
        else: continue
        M = mul(M, N)
    return M
def mul(A, B):
    a, b, c, d, e, f = A; g, h, i, j, k, l = B
    return (a*g + c*h, b*g + d*h, a*i + c*j, b*i + d*j, a*k + c*l + e, b*k + d*l + f)
def ap(M, x, y): return (M[0]*x + M[2]*y + M[4], M[1]*x + M[3]*y + M[5])
def arc_samples(x1, y1, rx, ry, phi, fa, fs, x2, y2):
    import math
    rx, ry = abs(rx), abs(ry)
    if rx < 1e-9 or ry < 1e-9: return [(x2, y2)]
    c, s = math.cos(math.radians(phi)), math.sin(math.radians(phi))
    dx, dy = (x1 - x2) / 2, (y1 - y2) / 2
    x1p, y1p = c * dx + s * dy, -s * dx + c * dy
    lam = x1p**2 / rx**2 + y1p**2 / ry**2
    if lam > 1: rx, ry = rx * math.sqrt(lam), ry * math.sqrt(lam)
    num = rx**2 * ry**2 - rx**2 * y1p**2 - ry**2 * x1p**2
    co = math.sqrt(max(0, num / (rx**2 * y1p**2 + ry**2 * x1p**2 or 1e-12))) * (-1 if fa == fs else 1)
    cxp, cyp = co * rx * y1p / ry, -co * ry * x1p / rx
    cx, cy = c * cxp - s * cyp + (x1 + x2) / 2, s * cxp + c * cyp + (y1 + y2) / 2
    ang = lambda ux, uy, vx, vy: math.atan2(ux * vy - uy * vx, ux * vx + uy * vy)
    t1 = ang(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry)
    dt = ang((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry)
    if not fs and dt > 0: dt -= 2 * math.pi
    elif fs and dt < 0: dt += 2 * math.pi
    out = []
    for k in range(33):
        t = t1 + dt * k / 32; ex, ey = rx * math.cos(t), ry * math.sin(t)
        out.append((c * ex - s * ey + cx, s * ex + c * ey + cy))
    return out
def path_pts(d):
    toks = re.findall(r'[MmLlHhVvCcSsQqTtAaZz]|' + NUM, d)
    out, i, cmd, x, y, sx, sy = [], 0, None, 0.0, 0.0, 0.0, 0.0
    ARG = dict(M=2, L=2, H=1, V=1, C=6, S=4, Q=4, T=2, A=7, Z=0)
    while i < len(toks):
        t = toks[i]
        if t.isalpha(): cmd = t; i += 1
        if cmd in 'Zz': x, y = sx, sy; cmd = None; continue
        if cmd is None: i += 1; continue
        n = ARG[cmd.upper()]; sl = toks[i:i + n]
        if len(sl) < n or any(t.isalpha() for t in sl):
            i += next((j for j, t in enumerate(sl) if t.isalpha()), len(sl)); cmd = None; continue
        a = [float(v) for v in sl]; i += n
        rel = cmd.islower(); U = cmd.upper()
        if U == 'H': x = a[0] + (x if rel else 0)
        elif U == 'V': y = a[0] + (y if rel else 0)
        elif U == 'A':
            nx, ny = a[5] + (x if rel else 0), a[6] + (y if rel else 0)
            out += arc_samples(x, y, a[0], a[1], a[2], a[3], a[4], nx, ny)
            x, y = nx, ny
        else:
            for k in range(0, n, 2):
                px, py = a[k] + (x if rel else 0), a[k + 1] + (y if rel else 0)
                out.append((px, py))
            x, y = out[-1]
        if U == 'M': sx, sy = x, y; cmd = 'l' if rel else 'L'
        out.append((x, y))
    return out
def check(fn):
    root = ET.parse(fn).getroot()
    vb = nums(root.get('viewBox'))
    if len(vb) != 4: return []
    X0, Y0, X1, Y1 = vb[0] - TOL, vb[1] - TOL, vb[0] + vb[2] + TOL, vb[1] + vb[3] + TOL
    bad = []
    def walk(el, M):
        tag = el.tag.split('}')[-1]
        if tag in ('defs', 'marker', 'style', 'title', 'desc', 'clipPath', 'mask', 'pattern', 'symbol', 'linearGradient', 'radialGradient', 'filter'): return
        M = mul(M, tf(el.get('transform')))
        g = lambda k: float(el.get(k, 0) or 0)
        P = []
        if tag == 'rect': x, y, w, h = g('x'), g('y'), g('width'), g('height'); P = [(x, y), (x + w, y + h)]
        elif tag == 'circle': P = [(g('cx') - g('r'), g('cy') - g('r')), (g('cx') + g('r'), g('cy') + g('r'))]
        elif tag == 'ellipse': P = [(g('cx') - g('rx'), g('cy') - g('ry')), (g('cx') + g('rx'), g('cy') + g('ry'))]
        elif tag == 'line': P = [(g('x1'), g('y1')), (g('x2'), g('y2'))]
        elif tag in ('polygon', 'polyline'): v = nums(el.get('points')); P = list(zip(v[::2], v[1::2]))
        elif tag == 'path': P = path_pts(el.get('d', ''))
        elif tag in ('text', 'tspan') and el.get('x') is not None and el.get('y') is not None and not nums(el.get('x'))[1:]:
            P = [(nums(el.get('x'))[0], nums(el.get('y'))[0])]
        for (px, py) in P:
            qx, qy = ap(M, px, py)
            if not (X0 <= qx <= X1 and Y0 <= qy <= Y1):
                bad.append(f'{tag} ({qx:.2f},{qy:.2f}) outside viewBox {vb}'); return
        if tag == 'foreignObject': return
        for c in el: walk(c, M)
    walk(root, (1, 0, 0, 1, 0, 0))
    return bad
if __name__ == '__main__':
    fails = 0
    for fn in sys.argv[1:]:
        try: b = check(fn)
        except ET.ParseError as e: b = [f'parse error {e}']
        if b: fails += 1; print(f'BOUNDS {fn}: {len(b)} violation(s); first: {b[0]}')
    print(f'bounds: {len(sys.argv) - 1 - fails} ok, {fails} failing'); sys.exit(1 if fails else 0)
