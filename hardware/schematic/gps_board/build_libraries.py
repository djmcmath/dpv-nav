"""Generate carrier footprints from Adafruit EAGLE board coordinates (mm).

Run with Python 3; no external packages. The carrier is viewed from its
component side, with the module component side facing the same direction.
"""
from pathlib import Path
import math
import xml.etree.ElementTree as ET
import json
import shutil
import re

ROOT = Path(__file__).resolve().parent
LIB = ROOT / 'GPS_Carrier.pretty'
LIB.mkdir(exist_ok=True)
RECORDS = {}

def num(v):
    return f'{v:.6f}'.rstrip('0').rstrip('.') if v else '0'

def xy(x,y):
    return f'{num(x)} {num(y)}'

def line(x1,y1,x2,y2,layer='F.Fab',width=.1):
    return f'(fp_line (start {xy(x1,y1)}) (end {xy(x2,y2)}) (stroke (width {width}) (type solid)) (layer "{layer}"))'

def rect(x1,y1,x2,y2,layer='F.CrtYd',width=.05):
    return f'(fp_rect (start {xy(x1,y1)}) (end {xy(x2,y2)}) (stroke (width {width}) (type solid)) (fill none) (layer "{layer}"))'

def txt(s,x,y,layer='F.Fab',size=.8,angle=0):
    return f'(fp_text user "{s}" (at {xy(x,y)} {angle}) (layer "{layer}") (effects (font (size {size} {size}) (thickness .12))))'

def module(filename,name,ox,oy,rows,extra_bounds):
    b=ET.parse(ROOT/'reference'/filename).getroot().find('./drawing/board')
    out=[f'(footprint "{name}" (version 20241229) (generator "pcbnew") (layer "F.Cu")',
         '(attr through_hole)',
         f'(descr "Carrier footprint; exact header centers from Adafruit {filename}. Mount above carrier using headers; not a flush SMT footprint.")',
         '(property "Reference" "REF**" (at 0 -13.8) (layer "F.SilkS") (effects (font (size 1 1) (thickness .15))))',
         f'(property "Value" "{name}" (at 0 14.5) (layer "F.Fab") (effects (font (size 1 1) (thickness .15))))']
    trans=lambda x,y:(float(x)-ox,oy-float(y))
    for w in b.findall('./plain/wire'):
        if w.get('layer')!='20': continue
        x1,y1=trans(w.get('x1'),w.get('y1')); x2,y2=trans(w.get('x2'),w.get('y2'))
        if w.get('curve'):
            # Arc midpoint from chord and signed sweep, with EAGLE y inverted.
            theta=-math.radians(float(w.get('curve')))
            dx,dy=x2-x1,y2-y1
            k=.5/math.tan(theta/2)
            cx,cy=(x1+x2)/2-dy*k,(y1+y2)/2+dx*k
            a=math.atan2(y1-cy,x1-cx)+theta/2
            r=math.hypot(x1-cx,y1-cy)
            xm,ym=cx+r*math.cos(a),cy+r*math.sin(a)
            for layer,width in [('F.Fab',.1),('F.SilkS',.12)]:
                out.append(f'(fp_arc (start {xy(x1,y1)}) (mid {xy(xm,ym)}) (end {xy(x2,y2)}) (stroke (width {width}) (type solid)) (layer "{layer}"))')
        else:
            out += [line(x1,y1,x2,y2),line(x1,y1,x2,y2,'F.SilkS',.12)]
    records=[]
    for elem,offset in rows:
        e=b.find(f'./elements/element[@name="{elem}"]')
        p=b.find(f'./libraries/library[@name="{e.get("library")}"]/packages/package[@name="{e.get("package")}"]')
        angle=math.radians(float(e.get('rot','R0').replace('R','')))
        for pad in p.findall('pad'):
            if not pad.get('name').isdigit(): continue
            n=int(pad.get('name'))+offset
            px,py=float(pad.get('x')),float(pad.get('y'))
            x,y=trans(float(e.get('x'))+px*math.cos(angle)-py*math.sin(angle),float(e.get('y'))+px*math.sin(angle)+py*math.cos(angle))
            net=next(s.get('name') for s in b.findall('./signals/signal') for c in s.findall('contactref') if c.get('element')==elem and c.get('pad')==pad.get('name'))
            shape='rect' if n==1 else 'circle'
            out.append(f'(pad "{n}" thru_hole {shape} (at {xy(x,y)}) (size 1.8 1.8) (drill 1) (layers "*.Cu" "*.Mask"))')
            records.append(dict(pad=n,signal=net,x=round(x,6),y=round(y,6),source_element=elem,source_pad=pad.get('name')))
            if filename.startswith('qtpy'):
                out.append(txt(net,x+(2.4 if x<0 else -2.4),y))
            elif filename=='gps-serial.brd':
                label={'3.3V':'3V3','VBACKUP':'VBAT','ENABLE':'EN','1PPS':'PPS'}.get(net,net)
                out.append(txt(label,x,y-2.8,angle=90))
            else:
                out.append(txt(net,x,y-1.8))
    holes=[]
    for e in b.findall('./elements/element'):
        if e.get('package')!='MOUNTINGHOLE_2.5_PLATED': continue
        x,y=trans(e.get('x'),e.get('y'))
        out.append(f'(pad "" np_thru_hole circle (at {xy(x,y)}) (size 2.5 2.5) (drill 2.5) (layers "*.Cu" "*.Mask"))')
        out.append(f'(fp_circle (center {xy(x,y)}) (end {xy(x+2.2,y)}) (stroke (width .1) (type solid)) (fill none) (layer "F.Fab"))')
        holes.append(dict(x=x,y=y,drill=2.5))
    out.append(rect(*extra_bounds))
    if filename.startswith('qtpy'):
        out += [rect(-4.5,-11.45,4.5,-3.9,'F.Fab',.1),txt('USB-C',0,-10.1),
                rect(-6.5,5.8,6.5,10.36,'Dwgs.User',.1),txt('ANTENNA / RF CLEARANCE',0,8.8,'Dwgs.User',.6)]
    elif filename=='gps-usbc.brd':
        out += [rect(-21.0,-4.5,-13.5,4.5,'F.Fab',.1),txt('ON-MODULE USB',-17.3,0,size=.6),
                txt('GPS antenna faces sky',3,0,'F.Fab',1),txt('Raised module: check battery and spacers',0,3)]
    else:
        out += [txt('MOUNTED GPS #746',0,-7,'F.Fab',1),txt('Antenna faces outward',0,-4,'F.Fab',.8),txt('9-pin header to carrier',0,9,'F.Fab',.7)]
    out.append(')')
    (LIB/(name+'.kicad_mod')).write_text('\n'.join(out)+'\n',encoding='utf-8')
    RECORDS[name]=dict(source=filename,pads=records,holes=holes,origin_eagle=[ox,oy])

module('qtpy-s2.brd','Adafruit_QT_Py_ESP32_S2_THT',8.89,10.3505,[('JP1',0),('JP3',7)],[-9.4,-11.95,9.4,10.9])
module('gps-serial.brd','Adafruit_Ultimate_GPS_Serial_746_THT',12.7,17.145,[('JP1',0)],[-13.2,-17.645,13.2,17.645])
# Raised assembly variant: allow low components beneath the module while
# reserving the header and each mounting spacer. Full envelope stays visible.
base='Adafruit_Ultimate_GPS_Serial_746_THT'
raised=(LIB/(base+'.kicad_mod')).read_text().replace(base,base+'_Raised')
raised=raised.replace('(layer "F.CrtYd")','(layer "Dwgs.User")')
raised=re.sub(r'\(fp_(?:line|arc).*?\(layer "F.SilkS"\)\)\n','',raised)
extra=[rect(-11.42,13.85,11.42,16.4)]
for hole in RECORDS[base]['holes']:
    x,y=hole['x'],hole['y']
    extra.append(f'(fp_circle (center {xy(x,y)}) (end {xy(x+2.2,y)}) (stroke (width .05) (type solid)) (fill none) (layer "F.CrtYd"))')
raised=raised.rstrip()[:-1]+'\n'+'\n'.join(extra)+'\n)\n'
(LIB/(base+'_Raised.kicad_mod')).write_text(raised)
KICAD=Path(r'D:\Program Files\KiCad\9.0\share\kicad\footprints')
for library,filename in [
    ('Connector_Molex','Molex_PicoBlade_53047-0410_1x04_P1.25mm_Vertical'),
    ('Connector_PinSocket_2.54mm','PinSocket_1x02_P2.54mm_Vertical'),
    ('Resistor_SMD','R_0603_1608Metric'),
    ('Diode_SMD','D_SOD-123'),
    ('Package_TO_SOT_SMD','SOT-23')]:
    shutil.copyfile(KICAD/(library+'.pretty')/(filename+'.kicad_mod'),LIB/(filename+'.kicad_mod'))
(ROOT/'reference'/'pad-map.json').write_text(json.dumps(RECORDS,indent=2)+'\n')
(ROOT/'fp-lib-table').write_text('(fp_lib_table (version 7)\n  (lib (name "GPS_Carrier")(type "KiCad")(uri "${KIPRJMOD}/GPS_Carrier.pretty")(options "")(descr "Local carrier footprints"))\n)\n')
print('Generated',len(list(LIB.glob('*.kicad_mod'))),'footprints and reference/pad-map.json')
