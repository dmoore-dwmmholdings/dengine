"""Deterministic original test art; CC0-1.0. Requires Pillow only to regenerate PNGs.
Direction: quiet/cool/layered dusk ruin; warm character/torch focus. 8px block rhythm,
crisp 1px detail, pale background silhouettes and dark foreground edges. No text/UI.
"""
from pathlib import Path
from PIL import Image, ImageDraw
P=Path(__file__).parent
(P/'textures').mkdir(exist_ok=True)
def save(name,size,fn):
 im=Image.new('RGBA',size); d=ImageDraw.Draw(im); fn(d,size); im.save(P/'textures'/f'{name}.png')
 return im
colors=[(18,25,40),(28,42,60),(40,61,78),(55,82,94),(77,109,114),(112,139,136),(164,175,154),(224,205,163),(89,62,57),(131,83,57),(183,114,61),(235,163,75),(255,212,118),(58,82,71),(82,111,79),(120,143,94)]
def sky(d,s):
 d.rectangle((0,0,*s),fill=colors[1]); d.ellipse((165,16,186,37),fill=colors[6]); d.rectangle((0,62,239,79),fill=colors[2])
 for x,y in [(12,14),(48,23),(91,8),(132,31),(218,15),(208,45)]: d.point((x,y),fill=colors[5])
save('sky',(240,135),sky)
def ridge(d,s):
 d.polygon([(0,95),(0,48),(22,26),(36,42),(61,12),(92,45),(120,29),(147,51),(181,19),(211,45),(239,26),(239,95)],fill=colors[3]);
 for x,h in [(12,20),(74,35),(126,25),(197,43)]:
  d.rectangle((x,95-h,x+9,95),fill=colors[4]); d.rectangle((x+2,95-h+4,x+4,95-h+8),fill=colors[2])
save('ruins',(240,96),ridge)
def stone(d,s):
 d.rectangle((0,0,*s),fill=colors[2]); d.rectangle((0,0,s[0],3),fill=colors[5])
 for y in range(6,s[1],8):
  d.line((0,y,s[0],y),fill=colors[1]);
  for x in range(-8 if y%16 else 0,s[0],16): d.line((x,y,x,y+7),fill=colors[1])
 for x in range(0,s[0],11): d.rectangle((x,0,x+4,1),fill=colors[14])
save('stone',(64,24),stone)
def hero(d,s):
 d.rectangle((10,2,21,13),fill=colors[7]); d.rectangle((8,1,23,5),fill=colors[10]); d.rectangle((8,6,11,15),fill=colors[9]); d.rectangle((20,7,22,8),fill=colors[0]); d.rectangle((7,15,24,31),fill=colors[10]); d.rectangle((4,18,8,31),fill=colors[9]); d.rectangle((24,18,27,29),fill=colors[7]); d.rectangle((8,31,15,43),fill=colors[1]); d.rectangle((18,31,24,43),fill=colors[1]); d.rectangle((5,42,15,45),fill=colors[0]); d.rectangle((18,42,27,45),fill=colors[0]); d.rectangle((12,17,15,21),fill=colors[12]); d.line((10,24,22,24),fill=colors[8])
hero_im=save('character',(32,48),hero)
norm=Image.new('RGBA',hero_im.size,(128,128,255,255)); n=ImageDraw.Draw(norm); n.rectangle((0,0,15,47),fill=(96,128,249,255)); n.rectangle((16,0,31,47),fill=(160,128,249,255)); norm.save(P/'textures/character_normal.png')
em=Image.new('RGBA',hero_im.size); ImageDraw.Draw(em).rectangle((12,17,15,21),fill=(255,178,60,255)); em.save(P/'textures/character_emissive.png')
def foliage(d,s):
 for x,y in [(6,23),(13,15),(22,25),(30,10),(39,21),(49,15),(57,25)]:
  d.line((x,47,x,y),fill=colors[13],width=2)
  for k in range(y,42,6):
   d.polygon([(x,k+5),(x-6,k),(x-2,k-1),(x,k+1)],fill=colors[14]); d.polygon([(x,k+7),(x+6,k+2),(x+2,k+1),(x,k+3)],fill=colors[15])
save('foliage',(64,48),foliage)
def torch(d,s):
 d.rectangle((6,17,9,31),fill=colors[8]); d.polygon([(5,18),(2,10),(6,5),(7,0),(13,9),(12,17)],fill=colors[10]); d.polygon([(6,17),(5,11),(9,5),(11,12),(9,18)],fill=colors[12])
save('torch',(16,32),torch)
def smoke(d,s):
 for x,y,r in [(18,48,13),(30,36,18),(20,20,15),(38,9,10)]:d.ellipse((x-r,y-r,x+r,y+r),fill=(112,139,136,65))
save('smoke',(64,64),smoke)
save('glass',(32,48),lambda d,s:(d.rectangle((1,1,30,46),fill=(67,153,166,95)),d.rectangle((1,1,3,46),fill=(164,215,216,145)),d.line((6,34,24,7),fill=(164,215,216,100),width=2)))
pal=Image.new('RGBA',(16,1)); pal.putdata([c+(255,) for c in colors]);pal.save(P/'textures/palette.png')
manifest=['[project]','asset_root = "."','','[assets]']
for file in sorted((P/'textures').glob('*.png')):
 extra=', usage="normal"' if file.stem.endswith('normal') else ''
 manifest.append(f'"tex/{file.stem}" = {{type="texture", source="textures/{file.name}", pixel_art=true, compression="none"{extra}}}')
manifest+=['"scene/pixel2d" = {type="scene",source="pixel2d.scene.toml"}','','[bundles.pixel2d]','assets = ["tex/*", "scene/*"]']
(P/'assets.toml').write_text('\n'.join(manifest)+'\n')
entities=[]
def entity(i,x,y,z,component,rotation='[0.7071067812,0.0,0.0,0.7071067812]'):
 entities.extend(['[[entity]]',f'id="0x{i:016x}"','[entity.DeTransform]',f'position=[{x},{y},{z}]',f'rotation={rotation}','scale=[1.0,1.0,1.0]',component,''])
entity(1,.011,-12,4.21875,'[entity.DeCamera]\nprojection="DE_PROJ_ORTHOGRAPHIC"\northo_height=8.4375\nnear_z=0.1\nexposure_ev100=0.0')
idx=10
def sprite(tex,x,y,z,w,h,extra='',color='[1.0,1.0,1.0,1.0]'):
 global idx
 idx+=1; entity(idx,x,y,z,f'[entity.DeSprite]\nuv_rect=[0.0,0.0,1.0,1.0]\ntexture="tex/{tex}"\nsize=[{w/32},{h/32}]\npivot=[0.5,0.5]\ncolor={color}\nthickness=0.125\ncast_shadows=true\n{extra}',rotation='[0.0,0.0,0.0,1.0]')
sprite('sky',0,6,4.21875,480,270,'emissive_tex="tex/sky"\ncast_shadows=false')
sprite('ruins',0,4,3.0,480,192,'emissive_tex="tex/ruins"\ncast_shadows=false', '[0.55,0.65,0.7,1.0]')
sprite('stone',0,2,0.375,480,24)
for x,z,w in [(-4.5,2.1,96),(0.5,3.2,80),(4.7,4.3,96)]:sprite('stone',x,1,z,w,16)
sprite('character',-.613,0,1.5,32,48,'normal_tex="tex/character_normal"\nemissive_tex="tex/character_emissive"')
for x,z in [(-4.7,2.85),(4.9,5.05)]:
 sprite('torch',x,.75,z,16,32,'emissive_tex="tex/torch"')
 entity(100+idx,x,-.2,z+.2,'[entity.DeLight]\nkind="DE_LIGHT_POINT"\ncolor=[1.0,0.55,0.18]\nintensity=90.0\nrange=7.0\nsource_radius=0.03\ncast_shadows=true')
for x in [-6.6,-2.8,3.1,6.7]:sprite('foliage',x,-.25,1.125,64,48)
sprite('glass',2.3,-.45,1.6,64,96,'translucent=true\nlayer=1\norder=2\ncast_shadows=false')
sprite('smoke',2.5,-.2,2.0,96,96,'translucent=true\nlayer=1\norder=1\ncast_shadows=false')
sprite('smoke',2.1,-.6,2.3,80,80,'translucent=true\nlayer=2\norder=0\ncast_shadows=false')
entity(200,0,-3,8,'[entity.DeLight]\nkind="DE_LIGHT_DIRECTIONAL"\ncolor=[0.65,0.76,1.0]\nintensity=8.0\ncast_shadows=true\nsource_radius=0.02', '[0.5,0.0,0.0,0.866025404]')
# Entity component field names must be unique TOML keys.
(P/'pixel2d.scene.toml').write_text('\n'.join(entities).replace('cast_shadows=true\nemissive_tex="tex/sky"\ncast_shadows=false','emissive_tex="tex/sky"\ncast_shadows=false').replace('cast_shadows=true\nemissive_tex="tex/ruins"\ncast_shadows=false','emissive_tex="tex/ruins"\ncast_shadows=false').replace('cast_shadows=true\ntranslucent=true','translucent=true'))
