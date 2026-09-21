from pathlib import Path
import argparse
import csv
import json
import math
import hashlib
import subprocess
import numpy as np

ROOT = Path('/tmp/nvenc-native-nv12-20260919')
SAMPLES = [0, 1, 2, 3, 300, 599]

def require(condition, detail):
    if not condition:
        raise RuntimeError(detail)

def read_exact(pipe, count):
    data = bytearray()
    while len(data) < count:
        part = pipe.read(count-len(data))
        if not part:
            break
        data.extend(part)
    return data

def validate(directory):
    movie = directory/'output.hevc'
    probe_command = ['ffprobe','-v','error','-select_streams','v:0','-count_packets',
                     '-show_entries','stream=codec_name,width,height,pix_fmt,color_range,nb_read_packets',
                     '-of','json',str(movie)]
    info = json.loads(subprocess.check_output(probe_command,text=True))['streams'][0]
    require(info['width']==4512 and info['height']==4512 and info['codec_name']=='hevc', info)
    require(int(info['nb_read_packets'])==600, info)
    require(info['pix_fmt'] in ('yuv420p','gray'), info)
    plane_size = info['width']*info['height']
    frame_size = plane_size*3//2 if info['pix_fmt']=='yuv420p' else plane_size
    rows=list(csv.DictReader((directory/'frames.csv').open()))
    require(len(rows)==600 and [int(r['frame_index']) for r in rows]==list(range(600)), 'CSV frame accounting mismatch')
    select='+'.join(f'eq(n\\,{n})' for n in SAMPLES)
    command=['ffmpeg','-v','error','-threads','2','-i',str(movie),'-vf','select='+select,
             '-vsync','0','-pix_fmt',info['pix_fmt'],'-f','rawvideo','pipe:1']
    metrics=[]
    with (directory/'decode.log').open('w') as stderr:
        process = subprocess.Popen(command,stdout=subprocess.PIPE,stderr=stderr)
        try:
            for index in SAMPLES:
                data = read_exact(process.stdout,frame_size)
                require(len(data)==frame_size, (index,len(data),frame_size))
                luma = np.frombuffer(data,dtype=np.uint8,count=plane_size)
                source=int(rows[index]['source'])
                reference=np.fromfile(directory/f'reference_source{source:04d}.y8',dtype=np.uint8)
                require(reference.size==plane_size, 'Reference geometry mismatch')
                diff=luma.astype(np.float32)-reference.astype(np.float32)
                mse=float(np.mean(diff*diff))
                mae=float(np.mean(np.abs(diff)))
                psnr=10*math.log10(255*255/mse) if mse else None
                chroma=np.frombuffer(data,dtype=np.uint8,offset=plane_size)
                metric={'frame_index':index,'source':source,'luma_mean':float(luma.mean()),
                        'luma_stddev':float(luma.std()),'mae':mae,'psnr_db':psnr,
                        'max_abs_error':float(np.abs(diff).max()),
                        'chroma_min':int(chroma.min()) if chroma.size else None,
                        'chroma_max':int(chroma.max()) if chroma.size else None}
                # Broad lossy-codec guard: catches black, wrong-plane, layout and wrong-source outputs.
                metric['pass']=(mae<12 and (psnr is None or psnr>25) and float(luma.std())>20
                                and (not chroma.size or (int(chroma.min())==128 and int(chroma.max())==128)))
                metrics.append(metric)
            require(process.stdout.read(1)==b'', 'Unexpected additional decoded sample')
            returncode=process.wait(timeout=30)
            require(returncode==0, returncode)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
    report={'stream':info,'sampled_frames':SAMPLES,'decode_command':command,'samples':metrics,
            'pass':all(r['pass'] for r in metrics),
            'scope':'Full-stream CPU decode; pixel comparison on six selected frames against exact source Y; lossy codec tolerance.'}
    (directory/'decoded_validation.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'run':directory.name,'pass':report['pass'],'samples':metrics}),flush=True)
    require(report['pass'], f'Decoded pixel check failed: {directory}')
    return report

if __name__=='__main__':
    parser=argparse.ArgumentParser(description='Decode all 600 frames and compare six samples with their exact luma sources.')
    parser.add_argument('--root',type=Path,default=ROOT)
    ROOT=parser.parse_args().root
    reports={}
    for directory in sorted(ROOT.iterdir()):
        if directory.is_dir() and (directory/'output.hevc').exists():
            reports[directory.name]=validate(directory)
    (ROOT/'decoded_validation.json').write_text(json.dumps(reports,indent=2)+'\n')
    # Explicitly gate timing conclusions on trace integrity and decoded content.
    comparison_path=ROOT/'comparison.json'
    if comparison_path.exists():
        comparison=json.loads(comparison_path.read_text())
        require(set(comparison)==set(reports), 'Comparison/validation run sets differ')
        for name, result in comparison.items():
            require(result.get('trace_valid',False), f'Incomplete CUDA trace: {name}')
            result['content_validated']=reports[name]['pass']
            result['comparison_valid']=result['trace_valid'] and reports[name]['pass']
            result['bitstream_sha256']=hashlib.sha256((ROOT/name/'output.hevc').read_bytes()).hexdigest()
            (ROOT/name/'summary.json').write_text(json.dumps(result,indent=2)+'\n')
        comparison_path.write_text(json.dumps(comparison,indent=2)+'\n')
