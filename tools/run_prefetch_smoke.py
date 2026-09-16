#!/usr/bin/env python3
import argparse, json, subprocess, tempfile
from pathlib import Path
from run_fast_lru import materialize

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--binary',default='./trace_bench')
    ap.add_argument('--trace',default='msr_hm_0')
    ap.add_argument('--limit',type=int,default=200000)
    ap.add_argument('--capacity',type=int,default=256*1024*1024)
    ap.add_argument('--latencies-us',default='0,1000,5000,10000,20000')
    ap.add_argument('--prefetchers',default='none,obl,stride,pg')
    ap.add_argument('--out',required=True)
    a=ap.parse_args(); binary=Path(a.binary).resolve(); rows=[]
    with tempfile.TemporaryDirectory(prefix='pf-smoke-') as td:
        trace=Path(td)/f'{a.trace}.oracle'; materialize(a.trace,a.limit,trace)
        for pf in a.prefetchers.split(','):
            cmd=[str(binary),'--trace',str(trace),'--name',a.trace,'--capacity',str(a.capacity),'--latencies-us',a.latencies_us,'--prefetcher',pf]
            if a.limit: cmd += ['--limit',str(a.limit)]
            cp=subprocess.run(cmd,text=True,capture_output=True)
            if cp.returncode: raise RuntimeError(f'{pf} failed:\n{cp.stderr}')
            row=json.loads([x for x in cp.stdout.splitlines() if x.startswith('{')][-1]); rows.append(row)
            print(json.dumps({'prefetcher':pf,'requests':row['requests'],'predictions':row['predictions'],'scenarios':row['scenarios']}),flush=True)
    out=Path(a.out);out.parent.mkdir(parents=True,exist_ok=True);out.write_text(json.dumps({'trace':a.trace,'limit':a.limit,'results':rows},indent=2)+'\n')
if __name__=='__main__': main()
