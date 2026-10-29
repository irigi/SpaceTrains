"""Attach an explicit audit status to every historical table cell and figure."""
from pathlib import Path
import re,json,csv
ROOT=Path(__file__).resolve().parent
source=(ROOT/'original_KeplerVariableISPRockets.tex').read_text()
source='\n'.join(line for line in source.splitlines() if not line.lstrip().startswith('%'))
audit=json.loads((ROOT/'jupiter_normalization_audit.json').read_text());lookup={round(r['angle_deg']):r for r in audit}
records=[]
for block in re.findall(r'\\begin\{table\*\}.*?\\end\{table\*\}',source,re.S):
 label=re.search(r'\\label\{(tab:[^}]+)\}',block)
 if not label:continue
 label=label.group(1)
 for line in block.splitlines():
  if not re.match(r'^-?\d+\s*&',line):continue
  fields=[a.strip().strip('{}') for a in line.split('\\\\')[0].split('&')]
  if len(fields)!=8:continue
  for rho,val in zip([.39,.72,1.52,5.2,9.58,19.22,30.05],fields[1:]):
   angle=int(fields[0]);r=dict(table=label,angle_deg=angle,radius_AU=rho,historical_time_days=val,status='unresolved_solver_output' if val=='N/A' else 'historical_trajectory_parameters_unavailable',reconstructed_time_days='',radius_min_AU='',exhaust_min_kms='',exhaust_max_kms='',endpoint_residual='',mass_error_kg='',normal_time_H_max_abs='')
   if label=='tab:transfer_times_final_anomaly' and rho==5.2 and angle in lookup:
    a=lookup[angle];r.update(status='independent_fixed_geometry_dry_mass_reconstruction',reconstructed_time_days=a['time_days'],radius_min_AU=a['radius_min_AU'],exhaust_min_kms=a['exhaust_min_kms'],exhaust_max_kms=a['exhaust_max_kms'],endpoint_residual=a['normalized_endpoint_residual'],mass_error_kg=a['final_mass_kg']-1e6,normal_time_H_max_abs=a['normal_time_H_max_abs'])
   records.append(r)
with (ROOT/'table_diagnostics.csv').open('w') as f:
 w=csv.DictWriter(f,fieldnames=list(records[0]));w.writeheader();w.writerows(records)
from collections import Counter
summary={'cells':len(records),'tables':dict(Counter(r['table'] for r in records)),'statuses':dict(Counter(r['status'] for r in records)),'genuine_infeasibility_certificates':0,'qualification':'Reconstructions do not identify every historical branch. Blank diagnostics mean unavailable, not zero. U cells remain unresolved; no feasibility lower bound was fabricated.'}
(ROOT/'audit_coverage.json').write_text(json.dumps(summary,indent=2));print(json.dumps(summary,indent=2))
