"""Generate an editable KiCad 9 schematic and self-contained symbol library."""
from pathlib import Path
import uuid, re, math, json

ROOT=Path(__file__).resolve().parent
KLIB=Path(r'D:\Program Files\KiCad\9.0\share\kicad\symbols')
def uid(name): return str(uuid.uuid5(uuid.NAMESPACE_URL,'tern/gps-carrier/'+name))
def q(s): return json.dumps(str(s))
def fx(size=1.27,hide=False): return f'(effects (font (size {size} {size}))'+(' (hide yes)' if hide else '')+')'
def prop(k,v,x,y,hide=False,angle=0): return f'(property {q(k)} {q(v)} (at {x} {y} {angle}) {fx(hide=hide)})'
def extract(lib,name):
    data=(KLIB/(lib+'.kicad_sym')).read_text(encoding='utf-8')
    start=data.index('(symbol '+q(name)+'\n')
    depth=0; quoted=False; escape=False
    for i in range(start,len(data)):
        c=data[i]
        if escape: escape=False; continue
        if c=='\\' and quoted: escape=True; continue
        if c=='"': quoted=not quoted
        if not quoted:
            if c=='(': depth+=1
            elif c==')':
                depth-=1
                if depth==0:return data[start:i+1]
    raise ValueError(name)

def box(name,ref,left,right,width=12.7):
    height=max(len(left),len(right))*2.54+2.54
    sy=(max(len(left),len(right))-1)*1.27
    s=[f'(symbol {q(name)} (pin_names (offset 1.016)) (in_bom yes) (on_board yes)',prop('Reference',ref,0,height/2+5.08),prop('Value',name,0,height/2+2.54),prop('Footprint','',0,0,True),prop('Datasheet','',0,0,True),f'(symbol "{name}_0_1" (rectangle (start {-width} {height/2}) (end {width} {-height/2}) (stroke (width .254) (type default)) (fill (type background))))',f'(symbol "{name}_1_1"']
    pins={}
    for side,items in [(-1,left),(1,right)]:
        for i,(number,label,typ) in enumerate(items):
            x=side*(width+5.08); y=sy-i*2.54; a=0 if side==-1 else 180
            s.append(f'(pin {typ} line (at {x} {y} {a}) (length 5.08) (name {q(label)} {fx(1.016)}) (number {q(number)} {fx(1.016)}))')
            pins[str(number)]=(x,y,a)
    s+=[')',')']
    return '\n'.join(s),pins,height

LIB={}; PINS={}; HEIGHT={}
def custom(name,ref,l,r,width=12.7): LIB[name],PINS[name],HEIGHT[name]=box(name,ref,l,r,width)
custom('GPS_Serial_746','U',[(2,'VIN','power_in'),(3,'GND','power_in'),(7,'VBAT','power_in'),(8,'EN','input'),(9,'3V3_OUT','power_out')],[(4,'RX','input'),(5,'TX','output'),(6,'FIX','output'),(1,'PPS','output')],12.7)
custom('QT_Py_ESP32_S2','U',[(14,'5V_IN','power_in'),(13,'GND','power_in'),(12,'3V3_OUT','power_out'),(8,'RX / GPIO16','input'),(7,'TX / GPIO5','output'),(5,'SDA / GPIO7','bidirectional'),(6,'SCL / GPIO6','bidirectional')],[(1,'A0 / GPIO18','bidirectional'),(2,'A1 / GPIO17','bidirectional'),(3,'A2 / GPIO9','bidirectional'),(4,'A3 / GPIO8','bidirectional'),(9,'SCK / GPIO36','bidirectional'),(10,'MISO / GPIO37','bidirectional'),(11,'MOSI / GPIO35','bidirectional')],17.78)
custom('NAV_PicoBlade_4','J',[],[(1,'+5V_NAV','passive'),(2,'GND','passive'),(3,'NAV_TX','passive'),(4,'NAV_RX','passive')],10.16)
custom('LED_SOCKET','J',[(1,'LED_ANODE (+)','passive'),(2,'LED_CATHODE (-)','passive')],[],12.7)
for library,name in [('Device','R'),('Device','D_Schottky'),('Transistor_BJT','MMBT3904'),('power','PWR_FLAG')]:
    LIB[name]=extract(library,name)
    parent=re.search(r'\(extends "([^"]+)"\)',LIB[name])
    if parent:
        LIB[name]=extract(library,parent[1]).replace(parent[1],name)
    # Pins have no nested parentheses before their coordinate.
    pins={}
    pattern=r'\(pin\s+\w+\s+\w+\s*\(at\s+([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)\).*?\(number\s+"([^"]+)"'
    for m in re.finditer(pattern,LIB[name],re.S):pins[m[4]]=tuple(float(m[i]) for i in [1,2,3])
    PINS[name]=pins; HEIGHT[name]=7.62

(ROOT/'GPS_Carrier.kicad_sym').write_text('(kicad_symbol_lib (version 20241209) (generator "kicad_symbol_editor")\n'+'\n'.join(LIB.values())+'\n)\n',encoding='utf-8')
(ROOT/'sym-lib-table').write_text('(sym_lib_table (version 7) (lib (name "GPS_Carrier")(type "KiCad")(uri "${KIPRJMOD}/GPS_Carrier.kicad_sym")(options "")(descr "Carrier module and circuit symbols")))\n')
rootid=uid('sheet')
out=[f'(kicad_sch (version 20250114) (generator "eeschema") (generator_version "9.0") (uuid {q(rootid)}) (paper "A4")',
 '(title_block (title "GPS / QT Py two-sided carrier") (date "2026-10-05") (rev "B - draft") (company "Tern") (comment 1 "U1 mounted TOP; U2 mounted BOTTOM"))',
 '(lib_symbols '+'\n'.join(s.replace('(symbol '+q(name),'(symbol '+q('GPS_Carrier:'+name),1) for name,s in LIB.items())+')']
PARTS=[]
def note(text,x,y,size=1.27):
    out.append(f'(text {q(text)} (at {x} {y} 0) (effects (font (size {size} {size})) (justify left top)) (uuid {q(uid(text))}))')
def add(ref,name,val,x,y,nets,foot='',angle=0,dnp=False,fields=None):
    ident=uid(ref); h=HEIGHT[name]
    field_angle=90 if angle in (90,270) else 0
    properties=[prop('Reference',ref,x,y-h/2-5.08,angle=field_angle),prop('Value',val,x,y-h/2-2.54,angle=field_angle),prop('Footprint','GPS_Carrier:'+foot if foot else '',x,y,True),prop('Datasheet','',x,y,True)]
    if fields:
        properties += [prop(k,v,x,y,True) for k,v in fields.items()]
    out.append(f'(symbol (lib_id {q("GPS_Carrier:"+name)}) (at {x} {y} {angle}) (unit 1) (in_bom {"no" if ref.startswith("#") else "yes"}) (on_board {"no" if ref.startswith("#") else "yes"}) (dnp {"yes" if dnp else "no"}) (uuid {q(ident)}) '+ '\n'.join(properties)+ ' '.join(f'(pin {q(n)} (uuid {q(uid(ref+"/"+n))}))' for n in PINS[name])+ f'(instances (project "gps_carrier" (path {q("/"+rootid)} (reference {q(ref)}) (unit 1)))))')
    a=math.radians(angle)
    for n,(px,py,pa) in PINS[name].items():
        xx=round(x+px*math.cos(a)-py*math.sin(a),6); yy=round(y-px*math.sin(a)-py*math.cos(a),6)
        net=nets.get(n)
        if net is None:
            out.append(f'(no_connect (at {xx} {yy}) (uuid {q(uid(ref+"nc"+n))}))')
            continue
        # Extend wire away from the symbol, then label at its endpoint.
        b=math.radians(pa+angle)
        ex=round(xx-7.62*math.cos(b),6); ey=round(yy+7.62*math.sin(b),6)
        out.append(f'(wire (pts (xy {xx} {yy}) (xy {ex} {ey})) (stroke (width 0) (type default)) (uuid {q(uid(ref+"wire"+n))}))')
        labangle=0 if abs(ex-xx)>0.01 else 90
        justify='right bottom' if ex<xx else 'left bottom'
        if labangle==90:justify='left bottom'
        out.append(f'(label {q(net)} (at {ex} {ey} {labangle}) (effects (font (size 1.016 1.016)) (justify {justify})) (uuid {q(uid(ref+"label"+n))}))')
    if not ref.startswith('#'):
        PARTS.append(dict(ref=ref,value=val,footprint=foot,symbol=name,nets=nets,uuid=ident,dnp=dnp,fields=fields or {}))

FPQT='Adafruit_QT_Py_ESP32_S2_THT'; FPGPS='Adafruit_Ultimate_GPS_Serial_746_THT_Raised'
R='R_0603_1608Metric'; D='D_SOD-123'
note('NAV INTERFACE',22.86,20.32,1.8)
add('J1','NAV_PicoBlade_4','Molex 53047-0410',38.1,45.72,{'1':'+5V_NAV','2':'GND','3':'NAV_TX','4':'GPS_TX'},'Molex_PicoBlade_53047-0410_1x04_P1.25mm_Vertical')
note('Pin order is a design assumption: verify cable.\nTX/RX names are from the main-nav perspective.\nUART logic: 3.3 V. Supply: regulated 5 V.',22.86,60.96)
note('QT PY POWER ISOLATION',91.44,20.32,1.8)
add('D1','D_Schottky','SS14',116.84,43.18,{'1':'+5V_QT','2':'+5V_NAV'},D,180)
note('GPS VIN connects directly to NAV 5 V.\nD1 blocks QT USB power returning to NAV.\nQT Py USB-C is for programming.\nPower the whole carrier while UART is connected.',88.9,66.04,1.016)
note('U1: COMPLETE GPS BOARD MOUNTED ON TOP',175.26,20.32,1.6)
add('U1','GPS_Serial_746','Adafruit Ultimate GPS serial #746',223.52,48.26,{'2':'+5V_NAV','3':'GND','4':'NAV_TX','5':'GPS_TX'},FPGPS)
note('9-pin board-to-board header + two mounting holes.\nSerial GPS: no USB connector or flying-wire taps.\nNAV TX -> GPS RX; GPS TX -> NAV RX + QT RX.\nEN uses onboard pull-up; VBAT uses onboard cell.',175.26,66.04,1.016)

note('DIRECT UART / RECEIVE-ONLY MONITOR',22.86,91.44,1.8)
add('R1','R','4.7k',53.34,116.84,{'1':'GPS_TX','2':'QT_RX'},R,90)
note('GPS TX feeds J1.4 directly and U2.RX through R1.\nQT Py TX is unconnected: monitor only.\nNo UART driver isolation or GPS rework required.\nHeader numbering is original Adafruit JP1 order:\n1=PPS, 2=VIN, 3=GND, 4=RX, 5=TX,\n6=FIX, 7=VBAT, 8=EN, 9=3V3.',22.86,137.16,1.016)
add('U2','QT_Py_ESP32_S2','Adafruit QT Py ESP32-S2 #5325',127,124.46,{'14':'+5V_QT','13':'GND','8':'QT_RX','1':'LED_CTRL'},FPQT)
note('U2: COMPLETE QT PY MOUNTED ON BOTTOM.\nTwo 7-pin headers; USB-C for programming.\nRX = GPIO16; A0 = GPIO18 drives LED.\nTX disconnected; disable Wi-Fi.\nFirmware must detect stale/no GPS data.',101.6,144.78,1.016)

note('OPTIONAL STATUS LED - LOW-SIDE DRIVER',180.34,91.44,1.8)
add('R2','R','220R',194.31,110.49,{'1':'+5V_QT','2':'LED_A'},R,90)
add('J2','LED_SOCKET','LED socket (optional)',245.11,115.57,{'1':'LED_A','2':'LED_K'},'PinSocket_1x02_P2.54mm_Vertical',dnp=True)
add('R3','R','2.2k',194.31,137.16,{'1':'LED_CTRL','2':'LED_BASE'},R,90)
add('Q1','MMBT3904','MMBT3904',257.81,140.97,{'1':'LED_BASE','2':'GND','3':'LED_K'},'SOT-23')
add('R4','R','100k',194.31,157.48,{'1':'LED_BASE','2':'GND'},R,90)
note('J2: 1 = anode (+), 2 = cathode (-).\n220 ohm: about 6-14 mA, LED dependent.\nSingle indicator LED; socket optional.',226.06,153.67,1.016)
for i,(net,x,y) in enumerate([('+5V_NAV',91.44,184.15),('+5V_QT',114.3,184.15),('GND',137.16,184.15)]):
    add('#FLG0'+str(i+1),'PWR_FLAG','PWR_FLAG',x,y,{'1':net})
out += ['(sheet_instances (path "/" (page "1")))',')']
(ROOT/'gps_carrier.kicad_sch').write_text('\n'.join(out)+'\n',encoding='utf-8')
(ROOT/'reference'/'design.json').write_text(json.dumps(dict(root_uuid=rootid,parts=PARTS),indent=2)+'\n')
print('Created gps_carrier.kicad_sch with',len(PARTS),'components')
