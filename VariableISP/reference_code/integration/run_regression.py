"""Run all eight archived packages in isolated copies; leave their sources intact."""
from pathlib import Path
import concurrent.futures, json, os, shutil, subprocess, sys, tempfile, time
HERE=Path(__file__).resolve().parent
ROOT=HERE.parent
PACKAGES=[
 ('block1_cw',['validate_block1.py'],'test_cw.cpp'),
 ('block2_sensitivities',['validate_block2.py','certify_nonconvex.py'],'test_sensitivities.cpp'),
 ('block3_exhaust_band',['validate.py'],'test_band.cpp'),
 ('block4_perihelion',['validate.py','certify_audit.py'],'test_perihelion.cpp'),
 ('block9_flyby',['validate.py'],'test_flyby.cpp'),
 ('block9b_powered',['validate.py'],'test_powered.cpp'),
 ('block5_optimality',['validate.py'],'test_optimality.cpp'),
 ('block6_escape_capture',['validate.py'],'test_escape.cpp')]
def run_package(spec):
 name,scripts,cpp=spec
 source=ROOT/name
 if not source.exists():source=ROOT/'handoff'/name
 started=time.perf_counter(); stages=[]; logs=[]
 env=dict(os.environ,OPENBLAS_NUM_THREADS='1',OMP_NUM_THREADS='1',MKL_NUM_THREADS='1')
 with tempfile.TemporaryDirectory(prefix=name+'_',dir=HERE/'results') as temp:
  work=Path(temp)/name
  shutil.copytree(source,work,ignore=shutil.ignore_patterns('__pycache__','*.aux','*.log','*.out','*.blg'))
  cmds=[[sys.executable,s] for s in scripts]+[['g++','-std=c++17','-O2','-Wall','-Wextra','-pedantic',cpp,'-o','test_binary'],['./test_binary']]
  for cmd in cmds:
   try:
    p=subprocess.run(cmd,cwd=work,env=env,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=240)
    code=p.returncode; log=p.stdout
   except subprocess.TimeoutExpired as e:code=124;log=str(e)
   stages.append({'command':cmd,'returncode':code});logs.append('$ '+' '.join(cmd)+'\n'+log)
   if code:break
 (HERE/'results'/f'{name}.log').write_text('\n'.join(logs))
 return {'package':name,'passed':all(x['returncode']==0 for x in stages),'stages':stages,'elapsed_seconds':time.perf_counter()-started}
if __name__=='__main__':
 (HERE/'results').mkdir(exist_ok=True)
 with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
  results=list(pool.map(run_package,PACKAGES))
 (HERE/'results'/'regression_summary.json').write_text(json.dumps(results,indent=2)+'\n')
 for r in results:print(r['package'], 'PASS' if r['passed'] else 'FAIL',round(r['elapsed_seconds'],2),flush=True)
 sys.exit(0 if all(r['passed'] for r in results) else 1)
