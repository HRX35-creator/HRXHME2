"""Prepare a regular-file input for profiling the exact production build worker."""
import argparse
import hashlib
import struct
from pathlib import Path
from benchmark import load_dataset
from metrics import write_json

p=argparse.ArgumentParser()
p.add_argument('--dataset',required=True)
p.add_argument('--out',required=True)
p.add_argument('--m',type=int,default=32)
p.add_argument('--ef-construction',type=int,default=200)
a=p.parse_args()
manifest,base,_,_,_=load_dataset(a.dataset)
out=Path(a.out)
out.parent.mkdir(parents=True,exist_ok=True)
with out.open('xb') as f:
    f.write(struct.pack('<6Q',0x31444c4955425048,len(base),base.shape[1],a.m,a.ef_construction,8))
    f.write(base.astype('<f4',copy=False).tobytes())
write_json(str(out)+'.json',{'dataset':manifest,'m':a.m,'ef_construction':a.ef_construction,
                           'sha256':hashlib.sha256(out.read_bytes()).hexdigest(),
                           'purpose':'GPU kernel profiling only; profiler time is not the SQL build time'})
print(out)
