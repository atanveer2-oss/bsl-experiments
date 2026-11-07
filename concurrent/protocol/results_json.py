#!/usr/bin/env python3
"""Summarise the collected Chameleon rows as one JSON document (used during the
campaign to follow the results as they arrived).

    python3 results_json.py concurrent/results > results.json

For every (mix, key range, distribution): the thread counts seen, and per structure
the median throughput (Mops/s), median path length and repetition count at each
thread count. Plus a ratio block: gated against classic, the splay-list and CBTree
at 64 threads. Medians over all repetitions collected so far.
"""
import csv, glob, json, statistics as st, sys, collections, datetime as dt

NAMES = {
    'bsl_gated': 'Gated rule (ours)', 'bsl_morris': 'Morris estimator (ours)', 'bsl_exact': 'Exact estimator (ours)',
    'bsl_classic': 'Classic, same code (control)', 'flexlist_locks': 'Splay-list', 'cbtree-concurrent': 'CBTree',
    'cbtree-concurrent-fair': 'CBTree, fair', 'skiplist_locks': 'Skip list, locks (fork)',
    'skiplist_lazy': 'Lazy skip list (fork)', 'bronson_pext_bst_occ': 'Bronson AVL',
}
ORDER = list(NAMES)
DISTS = ['uniform', '90-10', '95-5', '99-1', 'zipf1']
DIST_NAMES = {'uniform': 'uniform', '90-10': '90/10', '95-5': '95/5', '99-1': '99/1', 'zipf1': 'Zipf 1'}

def main(root):
    rows = []
    for f in glob.glob(f'{root}/*/throughput_tacc_*.csv'):
        rows += list(csv.DictReader(open(f)))
    cell = collections.defaultdict(list)
    pk = {}
    for r in rows:
        cell[(r['mix'], r['key_range'], r['distribution'], r['structure'], int(r['threads']))].append(r)
        if r['structure'] == 'bsl_gated': pk = {'p': r['p'], 'K': r['K']}
    out = {'generated': dt.datetime.now(dt.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'), 'rows': len(rows),
           'build': pk, 'blocks': [], 'ratios': []}
    mixes = [m for m in ('100-0-0', '80-10-10') if any(k[0] == m for k in cell)] + sorted({k[0] for k in cell} - {'100-0-0', '80-10-10'})
    krs = sorted({k[1] for k in cell}, key=int)
    for mix in mixes:
        for kr in krs:
            for d in DISTS:
                keys = [k for k in cell if k[0] == mix and k[1] == kr and k[2] == d]
                if not keys: continue
                threads = sorted({k[4] for k in keys})
                series = []
                for s in ORDER:
                    pts = []
                    for t in threads:
                        v = cell.get((mix, kr, d, s, t))
                        if not v: pts.append(None); continue
                        pts.append({'mops': round(st.median(float(r['throughput_ops_s']) for r in v) / 1e6, 2),
                                    'path': round(st.median(float(r['path_length']) for r in v), 1),
                                    'n': len(v)})
                    if any(pts): series.append({'id': s, 'name': NAMES[s], 'ours': s.startswith('bsl_'), 'points': pts})
                out['blocks'].append({'mix': mix, 'key_range': int(kr), 'dist': d, 'dist_name': DIST_NAMES[d],
                                      'threads': threads, 'series': series})
                def med(s, t=64):
                    v = cell.get((mix, kr, d, s, t)); return st.median(float(r['throughput_ops_s']) for r in v) if v else None
                g = med('bsl_gated')
                if g:
                    rat = {'mix': mix, 'key_range': int(kr), 'dist': d, 'dist_name': DIST_NAMES[d], 'threads': 64, 'gated': round(g / 1e6, 1)}
                    for s, key in (('bsl_classic', 'classic'), ('flexlist_locks', 'splay'), ('cbtree-concurrent', 'cbtree'), ('skiplist_locks', 'locks')):
                        o = med(s); rat[key] = round(g / o, 2) if o else None
                    pg = cell.get((mix, kr, d, 'bsl_gated', 64)); pc = cell.get((mix, kr, d, 'bsl_classic', 64))
                    rat['path_gated'] = round(st.median(float(r['path_length']) for r in pg), 1)
                    rat['path_classic'] = round(st.median(float(r['path_length']) for r in pc), 1) if pc else None
                    out['ratios'].append(rat)
    json.dump(out, sys.stdout, separators=(',', ':'))

if __name__ == '__main__':
    main(sys.argv[1])
