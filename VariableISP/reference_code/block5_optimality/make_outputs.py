"""Generate display and C++ fixture values from validate.py output."""
import json
from pathlib import Path
p=Path(__file__).parent
j=json.loads((p/'test_cases.json').read_text());c=j['case2_budget_branch'];m=c['tube'];n=c['nominal']
args=[c['h'],n['U'],c['b_input'],c['tube_delta'],c['pmax_input']]
(p/'fixture_tube.inc').write_text('inline Tube fixture_tube(){return tube('+','.join(format(x,'.17g') for x in args)+',{'+','.join(format(x,'.17g') for x in n['residual_l2'])+'});}\n')
rows=[('Reconstructed effort $U$',n['U']),('Containment margin',m['containment_margin']),('$\\gamma$',m['gamma']),('$\\eta$',m['eta']),('Estimated gap $\\eta^2/(4\\gamma)$',m['gap_bound']),('Cheap global lower estimate',c['cheap']['lower_bound']),('Synthetic $t_c$',j['case3_conjugate_regression']['first_conjugate_time'])]
t='\\begin{table}[tb]\n\\centering\\small\n\\begin{tabular}{lr}\\toprule\nQuantity & Numerical value\\\\\\midrule\n'
for label,v in rows:
    if v<1e-4:
        mant,exp=f'{v:.6e}'.split('e');val=f'${mant}\\times10^{{{int(exp)}}}$'
    else: val=f'${v:.12g}$'
    t+=label+' & '+val+'\\\\\n'
t+='\\bottomrule\\end{tabular}\n\\caption{Floating-point fixtures. The nonzero-effort row is not an interval certificate.}\\label{tab:b5numbers}\n\\end{table}\n'
(p/'block5_numbers.tex').write_text(t)

section=p/'block5_section.tex'
s=section.read_text()
import re
block='% BEGIN GENERATED NUMBERS\n'+t+'% END GENERATED NUMBERS'
if '% BEGIN GENERATED NUMBERS' in s:
    s=re.sub(r'% BEGIN GENERATED NUMBERS.*?% END GENERATED NUMBERS', lambda m:block, s, flags=re.S)
else:
    s=s.replace(r'\input{block5_numbers}',block)
section.write_text(s)
