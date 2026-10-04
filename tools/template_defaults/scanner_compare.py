#!/usr/bin/env python3
"""Compare the template's `scanner` section with the scanner's own defaults.

Usage: scanner_compare.py REPO_ROOT TEMPLATE.json

The scanner reads its section as raw JSON (scanner.cpp loadConfigFromPath), so
the GovernanceRules dumper cannot see it. Its defaults live in three places:
ScanConfig initialisers (scan/output, hard-coded below from include/naab/scanner.h
-- re-check them if that header changes), ScannerEngine::isEnabled()/getLevel()
(absent check = enabled, "soft"), and the default_val argument at each
getNumOption() call site in src/scanner/checks_*.cpp, extracted by regex.
"""
import json, re, glob, sys
R=sys.argv[1]
t=json.load(open(sys.argv[2]))['scanner']
# collect check ids per category from source
enabled_calls = {}   # (cat, check) -> True
num_defaults = {}    # (cat, check, key) -> default
list_reads = set()
for f in glob.glob(R+'/src/scanner/checks_*.cpp'):
    s=open(f).read()
    m=re.search(r'const std::string CAT = "([a-z_]+)"', s)
    if not m: continue
    cat=m.group(1)
    for c in re.findall(r'(?:isEnabled|getLevel|addIssue)\(\s*CAT\s*,\s*"([a-z_0-9]+)"', s): enabled_calls[(cat,c)]=True
    for c,k,d in re.findall(r'getNumOption\(\s*CAT\s*,\s*"([a-z_0-9]+)"\s*,\s*"([a-z_0-9]+)"\s*,\s*([-0-9.]+)\)', s): num_defaults[(cat,c,k)]=float(d)
    for c in re.findall(r'get(?:Num)?ListOption\(\s*CAT\s*,\s*"([a-z_0-9]+)"', s): list_reads.add((cat,c))
scan_defaults = {'max_files':200,'max_depth':32,'max_file_size_kb':500,'include_tests':False,'follow_symlinks':False,'exclude_patterns':[]}
out_defaults = {'format':'text','max_issues_per_file':50,'max_total_issues':500,'group_by':'file','sort_by':'severity','show_line_preview':True,'show_fix_suggestion':True,'save_json':True,'save_text':True,'save_sarif':False,'json_path':'quality-report.json','text_path':'quality-report.txt','sarif_path':'quality-report.sarif'}
rows=[]
def cmp(path, tv, dv, note=''):
    if tv != dv: rows.append((path, tv, dv, note))
cmp('scanner.version', t.get('version'), '1.0'); cmp('scanner.mode', t.get('mode'), 'enforce')
for k,v in t.get('scan',{}).items():
    if k.startswith('_'): continue
    cmp('scanner.scan.'+k, v, scan_defaults.get(k, '<UNREAD>'))
for k,v in t.get('output',{}).items():
    if k.startswith('_'): continue
    cmp('scanner.output.'+k, v, out_defaults.get(k, '<UNREAD>'))
cats = [(c, t[c], c) for c in ['redundancy','code_quality','complexity','style','security'] if c in t]
cats += [('lang_rules.'+l, v, 'lang_'+l) for l,v in t.get('lang_rules',{}).items() if not l.startswith('_')]
unknown_checks=[]
for label, blk, cat in cats:
    if not isinstance(blk, dict): continue
    for chk, cfg in blk.items():
        if chk.startswith('_') or not isinstance(cfg, dict): continue
        if (cat, chk) not in enabled_calls: unknown_checks.append(f'scanner.{label}.{chk}')
        for k, v in cfg.items():
            if k.startswith('_'): continue
            p=f'scanner.{label}.{chk}.{k}'
            if k=='enabled': cmp(p, v, True)
            elif k=='level': cmp(p, v, 'soft')
            elif isinstance(v,(int,float)) and not isinstance(v,bool):
                d = num_defaults.get((cat,chk,k))
                if d is None: rows.append((p, v, '<UNREAD>', 'no getNumOption call'))
                elif float(v)!=d: rows.append((p, v, d, ''))
            elif isinstance(v,list):
                if (cat,chk) not in list_reads: rows.append((p, v, '<UNREAD>', 'no list read'))
                else: rows.append((p, v, '[] (call site may fall back)', 'list'))
            else: rows.append((p, v, '<UNREAD?>', type(v).__name__))
print('checks in template with no scanner implementation:', len(unknown_checks)); print('\n'.join(unknown_checks))
print(); print('rows:', len(rows))
for r in rows: print('\t'.join(map(str,r)))
