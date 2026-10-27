"""Offline authored-box geometry check; independent of SSR history-plane reconstruction."""
from pathlib import Path
import collections,json,math,struct,sys,tomllib,os
root=Path(sys.argv[1]);out=Path(sys.argv[2]);repo=Path.cwd()
meta=json.loads((root/'metadata.json').read_text());w,h=meta['width'],meta['height'];tw,th=meta['trace_width'],meta['trace_height'];raw=(root/'scene.bin').read_bytes();assert len(raw)==432
floats=struct.unpack('<96f',raw[:384]);inv=floats[48:64];origin=[floats[84+i]+floats[88+i] for i in range(3)];eye=[floats[80+i]+origin[i] for i in range(3)]
scene=tomllib.loads((repo/'tests/golden/base_tier/assets/scenes/reflections_ssr_probes.scene.toml').read_text());boxes=[]
for e in scene['entity']:
 if 'DeMeshRenderer' not in e:continue
 mr=e['DeMeshRenderer'];xf=e['DeTransform'];assert mr['mesh']=='mesh/box#box' and xf['rotation']==[0.,0.,0.,1.]
 pos=xf['position'];scale=xf['scale'];assert all(x>0 for x in scale)
 for mat in mr['materials']:
  material=tomllib.loads((repo/'tests/golden/base_tier/assets/materials'/(mat.split('/')[-1]+'.mat.toml')).read_text());assert not any('normal' in k for k in material)
 boxes.append({'id':e['id'],'lo':[pos[i]-scale[i]*.5 for i in range(3)],'hi':[pos[i]+scale[i]*.5 for i in range(3)]})
assert len(boxes)==7
camera=next(e for e in scene['entity'] if 'DeCamera' in e)['DeTransform']['position']
if os.getenv('DE_DENG604_MOTION'):camera[0]=.35*math.sin((95-48)*.11)
assert max(abs(a-b) for a,b in zip(camera,eye))<1e-4,(camera,eye)
def texture(name,index,fmt,width,height):
 data=(root/(name+'.bin')).read_bytes();pitch=meta['row_pitches'][index];stride=struct.calcsize('<'+fmt);assert len(data)==pitch*height
 return [struct.unpack_from('<'+fmt,data,y*pitch+x*stride) for y in range(height) for x in range(width)]
depth=texture('depth',0,'f',w,h);motion=texture('motion',2,'2e',w,h);hits=texture('hits',3,'4f',tw,th);surface=texture('history_surface',6,'4f',w,h)
def unproject(x,y,z):
 p=[2*x-1,1-2*y,z,1];v=[sum(inv[r*4+k]*p[k] for k in range(4)) for r in range(4)]
 return [v[i]/v[3]+origin[i] for i in range(3)]
def intersection(x,y):
 near=unproject(x,y,1);direction=[near[i]-eye[i] for i in range(3)];best=None
 for box in boxes:
  t0=-math.inf;t1=math.inf;face=None;valid=True
  for axis in range(3):
   d=direction[axis]
   if abs(d)<1e-15:
    if eye[axis]<box['lo'][axis] or eye[axis]>box['hi'][axis]:valid=False;break
    continue
   lo=(box['lo'][axis]-eye[axis])/d;hi=(box['hi'][axis]-eye[axis])/d;sign=-1;plane=box['lo'][axis]
   if lo>hi:lo,hi=hi,lo;sign=1;plane=box['hi'][axis]
   if lo>t0:t0=lo;face=(box['id'],axis,sign,plane)
   t1=min(t1,hi)
   if t0>t1:valid=False;break
  if valid and t0>0 and (best is None or t0<best[0]):best=(t0,face,[eye[i]+t0*direction[i] for i in range(3)])
 return best
faces=[];position_errors=[];missing_geometry=0;missing_raster=0
for y in range(h):
 for x in range(w):
  hit=intersection((x+.5)/w,(y+.5)/h);faces.append(hit[1] if hit else None);z=depth[y*w+x][0]
  if z>0 and hit:
   point=unproject((x+.5)/w,(y+.5)/h,z);position_errors.append(max(abs(a-b) for a,b in zip(point,hit[2])))
  elif z>0:missing_geometry+=1
  elif hit:missing_raster+=1
static_matrices=list(floats[32:48])==list(floats[64:80]);max_motion=max(abs(v) for p in motion for v in p);surface_depth_mismatch=sum(a[0]!=b[0] for a,b in zip(depth,surface))
previous_faces=faces
previous_depth_errors=[]
if os.getenv('DE_DENG604_MOTION'):
 assert not static_matrices and max_motion>1e-5 and surface_depth_mismatch>100
 def inverse4(matrix):
  rows=[[float(matrix[r*4+c]) for c in range(4)]+[float(r==c) for c in range(4)] for r in range(4)]
  for k in range(4):
   pivot=max(range(k,4),key=lambda i:abs(rows[i][k]));assert abs(rows[pivot][k])>1e-12
   rows[k],rows[pivot]=rows[pivot],rows[k];scale=rows[k][k];rows[k]=[v/scale for v in rows[k]]
   for i in range(4):
    if i!=k:
     scale=rows[i][k];rows[i]=[a-scale*b for a,b in zip(rows[i],rows[k])]
  return [v for row in rows for v in row[4:]]
 current_inv,current_eye=inv,eye
 inv=inverse4(floats[64:80]);eye=[inv[r*4+2]/inv[14] for r in range(3)]
 expected_previous=[.35*math.sin((94-48)*.11),camera[1],camera[2]]
 assert max(abs(a-b) for a,b in zip(eye,expected_previous))<1e-3,(eye,expected_previous)
 previous_faces=[];previous_missing_geometry=previous_missing_raster=0
 for y in range(h):
  for x in range(w):
   hit=intersection((x+.5)/w,(y+.5)/h);previous_faces.append(hit[1] if hit else None);z=surface[y*w+x][0]
   if z>0 and hit:
    point=unproject((x+.5)/w,(y+.5)/h,z);previous_depth_errors.append(max(abs(a-b) for a,b in zip(point,hit[2])))
   elif z>0:previous_missing_geometry+=1
   elif hit:previous_missing_raster+=1
 assert previous_missing_geometry==previous_missing_raster==0
 assert max(previous_depth_errors)<.003
 inv,eye=current_inv,current_eye
counts=collections.Counter();coverage=collections.Counter();examples=[]
# Use current authored visible faces as history geometry only for an actually stationary camera.
if static_matrices and max_motion<=1e-5 and surface_depth_mismatch==0:
 for index,hit in enumerate(hits):
  u,v,z,confidence=hit
  if not(z>0 and confidence>0):continue
  counts['positive_hits']+=1;hx=min(int(u*w),w-1);hy=min(int(v*h),h-1)
  if hx<0 or hy<0:counts['out_of_range_hits']+=1;continue
  face=faces[hy*w+hx]
  if face is None:counts['no_authored_source_face']+=1;continue
  mx,my=motion[hy*w+hx];uv=[u-mx*.5,v+my*.5];px=uv[0]*w-.5;py=uv[1]*h-.5;bx=math.floor(px);by=math.floor(py);fx=px-bx;fy=py-by;valid_weight=0;foreign=[]
  for dy in range(2):
   for dx in range(2):
    x=bx+dx;y=by+dy;weight=(fx if dx else 1-fx)*(fy if dy else 1-fy)
    if weight<=1e-9:continue
    other=faces[y*w+x] if 0<=x<w and 0<=y<h else None
    coplanar=other is not None and other[1:3]==face[1:3] and abs(other[3]-face[3])<1e-4
    if coplanar:valid_weight+=weight
    else:foreign.append({'xy':[x,y],'weight':weight,'face':other})
  bucket='full' if valid_weight>1-1e-6 else 'none' if valid_weight<1e-6 else 'partial';coverage[bucket]+=1
  if bucket!='full' and len(examples)<40:examples.append({'trace_pixel':[index%tw,index//tw],'hit_uv':[u,v],'source_face':face,'coplanar_coverage':valid_weight,'foreign_taps':foreign})
position_errors.sort();report={'scene':'authored7axisalignedboxes/unitboxhalfextent.5/noNormalMaps','capture_dir':str(root),'camera':eye,'origin':origin,'depth_geometry_check':{'compared_pixels':len(position_errors),'max_world_error':position_errors[-1] if position_errors else None,'p99_world_error':position_errors[math.ceil(.99*len(position_errors))-1] if position_errors else None,'within_1mm':sum(v<=.001 for v in position_errors),'raster_depth_without_authored_box':missing_geometry,'authored_box_without_raster_depth':missing_raster},'history_sanity':{'camera_matrices_static':static_matrices,'max_motion_ndc':max_motion,'current_vs_previous_depth_mismatches':surface_depth_mismatch},'hit_counts':dict(counts),'authored_coplanar_footprints':dict(coverage),'examples':examples,'scope':'Independent authored box-face geometry, not a proof of final radiance or ANC motion quality; do not use if image identity/source audit failed'}
out.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps({k:v for k,v in report.items() if k!='examples'},indent=2))
