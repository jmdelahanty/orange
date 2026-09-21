from pathlib import Path
import argparse
import csv
import hashlib
import json
import sqlite3
import statistics
import subprocess
import time

ROOT = Path('/tmp/nvenc-native-nv12-20260919')
BINARY = Path('/tmp/build-nvenc-native-probe/nvenc_native_nv12_probe')
NSYS = '/home/jeremy/.local/opt/nsight-systems-2025.5.2-nvenc/bin/nsys'

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def stats(values):
    values = sorted(values)
    return {'count':len(values), 'mean_ms':statistics.mean(values) if values else 0,
            'p50_ms':values[len(values)//2] if values else 0,
            'p95_ms':values[min(len(values)-1,int(len(values)*.95))] if values else 0,
            'max_ms':max(values,default=0)}

def summarize(directory):
    log_text = (directory/'run.log').read_text()
    errors = ('TargetProfilingFailed', 'Unknown driver API function',
              'Errors occurred while processing raw events', 'non-fatal errors')
    if any(marker in log_text for marker in errors):
        raise RuntimeError(f'Profiler reported incomplete data: {directory}/run.log')
    db = sqlite3.connect(directory/'trace.sqlite')
    tables = {x[0] for x in db.execute("SELECT name FROM sqlite_master WHERE type='table'")}
    kernel_groups = []
    if 'CUPTI_ACTIVITY_KIND_KERNEL' in tables:
        for name,count,total,gx,gy,gz,bx,by,bz in db.execute('''
            SELECT s.value,COUNT(*),SUM(k.end-k.start)/1e6,
                   k.gridX,k.gridY,k.gridZ,k.blockX,k.blockY,k.blockZ
            FROM CUPTI_ACTIVITY_KIND_KERNEL k JOIN StringIds s ON k.shortName=s.id
            GROUP BY s.value,k.gridX,k.gridY,k.gridZ,k.blockX,k.blockY,k.blockZ'''):
            kernel_groups.append({'name':name,'count':count,'total_ms':total,
                                  'grid':[gx,gy,gz],'block':[bx,by,bz]})
    memcpy_groups = []
    if 'CUPTI_ACTIVITY_KIND_MEMCPY' in tables:
        for kind,n,byte_count,count,total in db.execute('''
            SELECT copyKind,srcKind,bytes,COUNT(*),SUM(end-start)/1e6
            FROM CUPTI_ACTIVITY_KIND_MEMCPY GROUP BY copyKind,srcKind,bytes'''):
            memcpy_groups.append({'kind':kind,'source_kind':n,'bytes_per_copy':byte_count,'count':count,'total_ms':total})
    rows = list(csv.DictReader((directory/'frames.csv').open()))
    measured = [r for r in rows if r['phase']=='measure']
    if len(rows) != 600 or len(measured) != 550 or [int(r['frame_index']) for r in rows] != list(range(600)):
        raise RuntimeError(f'Unexpected CSV frame accounting: {directory}')
    expected_copies = 600 if rows[0]['update'] == 'per-frame' else 4
    copy_kind = 7 if rows[0]['input'] == 'native-array' else 8
    actual_copies = sum(g['count'] for g in memcpy_groups if g['kind']==copy_kind and g['bytes_per_copy']==4512*4512)
    if actual_copies != expected_copies or 'CUPTI_ACTIVITY_KIND_RUNTIME' not in tables:
        raise RuntimeError(f'Incomplete CUDA activity: expected {expected_copies} Y copies, got {actual_copies}')
    api_count = db.execute('SELECT COUNT(*) FROM CUPTI_ACTIVITY_KIND_RUNTIME').fetchone()[0]
    last_api_names = [r[0] for r in db.execute('SELECT s.value FROM CUPTI_ACTIVITY_KIND_RUNTIME k JOIN StringIds s ON k.nameId=s.id ORDER BY k.start DESC LIMIT 10')]
    if api_count == 0 or not any('cuStreamDestroy' in name for name in last_api_names):
        raise RuntimeError(f'CUDA trace does not extend through resource teardown: {directory}')
    diagnostics = [dict(zip(['severity','text'],r)) for r in db.execute('SELECT severity,text FROM DIAGNOSTIC_EVENT WHERE severity>=2')]
    if any(r['severity']>=3 for r in diagnostics):
        raise RuntimeError(f'Profiler reported an error: {diagnostics}')
    metrics = {key:stats([float(r[key]) for r in measured]) for key in
               ['copy_wall_ms','copy_gpu_ms','encode_wall_ms','map_ms','encode_picture_ms',
                'completion_wait_ms','lock_bitstream_ms','bitstream_copy_ms','unlock_bitstream_ms','unmap_ms']}
    metrics['copy_plus_encode_wall_ms'] = stats([float(r['copy_wall_ms'])+float(r['encode_wall_ms']) for r in measured])
    # With extra-output-delay=3, lock/copy/unmap retire an older input frame.
    output_measured = [r for r in measured if int(r['frame_index']) >= 53]
    output_metrics = {key:stats([float(r[key]) for r in output_measured]) for key in
                      ['completion_wait_ms','lock_bitstream_ms','bitstream_copy_ms','unlock_bitstream_ms','unmap_ms']}
    validation_path = directory/'decoded_validation.json'
    content_valid = json.loads(validation_path.read_text()).get('pass',False) if validation_path.exists() else False
    report = {'submitted_rows':len(rows),'measured_rows':len(measured),'kernel_groups':kernel_groups,
              'memcpy_groups':memcpy_groups,'total_kernel_count':sum(k['count'] for k in kernel_groups),
              'total_kernel_ms':sum(k['total_ms'] for k in kernel_groups),
              'conversion_kernel_count':sum(k['count'] for k in kernel_groups if 'Convert_' in k['name']),
              'conversion_ms_per_submitted_frame':sum(k['total_ms'] for k in kernel_groups if 'Convert_' in k['name'])/len(rows),
              'metrics':metrics,'output_stage_metrics_after_delayed_warmup':output_metrics,
              'trace_valid':True,'content_validated':content_valid,'comparison_valid':content_valid,
              'trace_integrity':{'expected_y_copies':expected_copies,'actual_y_copies':actual_copies,
                                 'api_rows':api_count,'last_api_names':last_api_names,'warnings':diagnostics},
              'limitations':['Single fixed-order pass; small wall-time deltas are descriptive, not a controlled causal estimate.',
                             'copy_wall_ms includes earlier NVENC work on the shared stream.',
                             'Filtered output-stage metrics cover retired frames 50-596; final three drain outside CSV.']}
    (directory/'summary.json').write_text(json.dumps(report,indent=2)+'\n')
    return report

def main():
    global ROOT, BINARY, NSYS
    parser = argparse.ArgumentParser(description='Matched standalone NVENC input-layout comparison on idle A16 GPU 1.')
    parser.add_argument('--root',type=Path,default=ROOT)
    parser.add_argument('--binary',type=Path,default=BINARY)
    parser.add_argument('--nsys',default=NSYS)
    args = parser.parse_args()
    ROOT, BINARY, NSYS = args.root,args.binary,args.nsys
    ROOT.mkdir(exist_ok=False)
    gpu = subprocess.check_output(['nvidia-smi','-i','1','--query-gpu=index,uuid,driver_version,utilization.gpu,memory.used','--format=csv,noheader'],text=True)
    (ROOT/'gpu-before.txt').write_text(gpu)
    # Confirm no compute owner on this die. Desktop processes on GPU0 are unrelated.
    processes = subprocess.check_output(['nvidia-smi','--query-compute-apps=gpu_uuid,pid,process_name','--format=csv,noheader'],text=True)
    (ROOT/'compute-processes-before.txt').write_text(processes)
    if 'GPU-c37c3690-5fbc-361c-5a77-8c525a47840f' in processes:
        raise RuntimeError('GPU1 has a compute owner; refusing comparison')
    (ROOT/'provenance.json').write_text(json.dumps({'binary':str(BINARY),'binary_sha256':sha(BINARY),
      'cuda_prefix':'/home/jeremy/.local/opt/cuda-13.1.1-nvenc',
      'production_cuda':str(Path('/usr/local/cuda').resolve()),
      'nsys':NSYS,'nsys_version':subprocess.check_output([NSYS,'--version'],text=True).strip(),
      'profiling_note':'CUDA-only trace includes initialization; CSV metrics exclude first 50 submitted frames. Output-stage timing excludes first 53 submissions for delay 3.'},indent=2)+'\n')
    summaries = {}
    for update in ['prefilled','per-frame']:
        for kind in ['linear','native-array']:
            label=f'{kind}_{update}'
            directory=ROOT/label
            directory.mkdir()
            command=[NSYS,'profile','--trace=cuda','--sample=none','--cpuctxsw=none',
                     '--output='+str(directory/'trace'), str(BINARY),'--gpu-id','1',
                     '--input',kind,'--update',update,'--frames','600','--warmup-frames','50',
                     '--source-frames','4','--fps','100','--extra-output-delay','3',
                     '--bitstream-out',str(directory/'output.hevc'),'--csv',str(directory/'frames.csv'),
                     '--reference-prefix',str(directory/'reference')]
            if kind == 'native-array':
                command += ['--native-storage-width','4608','--native-register-pitch','4608']
            (directory/'command.json').write_text(json.dumps(command,indent=2)+'\n')
            with (directory/'run.log').open('w') as log:
                result=subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,timeout=180)
            if result.returncode:
                raise RuntimeError(f'{label} failed; inspect {directory}/run.log')
            with (directory/'export.log').open('w') as log:
                subprocess.run([NSYS,'export','--type','sqlite','--output',str(directory/'trace.sqlite'),str(directory/'trace.nsys-rep')],stdout=log,stderr=subprocess.STDOUT,check=True,timeout=90)
            summaries[label]=summarize(directory)
            print(json.dumps({'completed':label,'summary':summaries[label]}),flush=True)
    (ROOT/'comparison.json').write_text(json.dumps(summaries,indent=2)+'\n')

if __name__=='__main__':
    main()
