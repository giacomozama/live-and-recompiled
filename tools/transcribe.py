#!/usr/bin/env python3
"""Whisper word timings for voice clips (input to tools/subtitle_times.py).

  python3 tools/transcribe.py MODEL 'WAV_GLOB' OUTDIR       e.g. large-v3 'out/speech/wav/*.wav' out/speech/json

Needs faster-whisper (a venv: python3 -m venv out/whisper-venv; out/whisper-venv/bin/pip install faster-whisper
nvidia-cublas-cu12 'nvidia-cudnn-cu12==9.*', with LD_LIBRARY_PATH at the venv's nvidia/cublas/lib and
nvidia/cudnn/lib) and ffmpeg, which decodes the clips (tools/xwb.py writes them). One JSON per clip; existing ones
are kept.
"""
import sys,json,os,glob,time,subprocess
import numpy as np
from faster_whisper import WhisperModel
model=WhisperModel(sys.argv[1], device="cuda", compute_type="float16")
files=sorted(glob.glob(sys.argv[2]))
out=sys.argv[3]; os.makedirs(out,exist_ok=True)
for f in files:
    dst=os.path.join(out,os.path.basename(f)[:-4]+'.json')
    if os.path.exists(dst): continue
    t=time.time()
    pcm=subprocess.run(['ffmpeg','-v','error','-i',f,'-ac','1','-ar','16000','-f','s16le','-'],capture_output=True,check=True).stdout
    audio=np.frombuffer(pcm,np.int16).astype(np.float32)/32768
    segs,info=model.transcribe(audio,language="en",word_timestamps=True,vad_filter=False,beam_size=5)
    res=[{'start':s.start,'end':s.end,'text':s.text,'words':[(w.start,w.end,w.word) for w in s.words]} for s in segs]
    json.dump(res,open(dst,'w'))
    print(os.path.basename(f), f'{info.duration:.0f}s in {time.time()-t:.1f}s', flush=True)
