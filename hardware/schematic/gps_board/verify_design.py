"""Cross-check KiCad's exported netlist against actual board pad assignments.
Run with KiCad's bundled Python after exporting reference/gps_carrier.net.xml.
"""
from pathlib import Path
import xml.etree.ElementTree as E
import pcbnew as p
import json, csv, hashlib

root=Path(__file__).resolve().parent
design=json.loads((root/'reference/design.json').read_text())
netlist=E.parse(root/'reference/gps_carrier.net.xml').getroot()
actual={}
for net in netlist.findall('./nets/net'):
    for node in net.findall('node'):
        actual[node.get('ref'),node.get('pin')]=net.get('name').removeprefix('/')
b=p.LoadBoard(str(root/'gps_carrier.kicad_pcb'))
fps={f.GetReference():f for f in b.GetFootprints()}
assert len(fps)==10
count=0
for part in design['parts']:
    ref=part['ref']; fp=fps[ref]
    pads={pad.GetNumber():pad for pad in fp.Pads() if pad.GetNumber()}
    for pin,net in part['nets'].items():
        assert actual[ref,pin]==net,(ref,pin,actual.get((ref,pin)),net)
        assert pads[pin].GetNetname()=='/'+net,(ref,pin,pads[pin].GetNetname(),net)
        count+=1
    for pin,pad in pads.items():
        if pin not in part['nets']:assert pad.GetNetCode()==0,(ref,pin)
    assert fp.GetPath().AsString()==f'/{design["root_uuid"]}/{part["uuid"]}'
# Explicit electrical invariants independent of symbol/placement generation.
assert actual['U2','8']=='QT_RX'
assert actual['R1','1']=='GPS_TX' and actual['R1','2']=='QT_RX'
assert actual['J1','4']=='GPS_TX' and actual['U1','5']=='GPS_TX'
assert actual['J1','3']=='NAV_TX' and actual['U1','4']=='NAV_TX'
assert actual['U2','1']=='LED_CTRL' and actual['Q1','2']=='GND'
assert actual['J2','1']=='LED_A' and actual['J2','2']=='LED_K'
assert actual['D1','2']=='+5V_NAV' # anode
assert actual['D1','1']=='+5V_QT' # cathode
assert actual['U1','2']=='+5V_NAV' and actual['U1','3']=='GND'
assert not {'J3','SJ1','D2'}.intersection(fps)
assert 'unconnected' in actual['U2','7'] # QT TX deliberately unused
qt=p.FootprintLoad(str(root/'GPS_Carrier.pretty'),'Adafruit_QT_Py_ESP32_S2_THT')
qp={d.GetNumber():d for d in qt.Pads()}
assert abs(p.ToMM(qp['1'].GetPosition().x-qp['14'].GetPosition().x))==15.24
assert abs(p.ToMM(qp['1'].GetPosition().y-qp['2'].GetPosition().y))==2.54
gps=p.FootprintLoad(str(root/'GPS_Carrier.pretty'),'Adafruit_Ultimate_GPS_Serial_746_THT')
gp={d.GetNumber():d for d in gps.Pads() if d.GetNumber()}
assert abs(p.ToMM(gp['1'].GetPosition().x-gp['2'].GetPosition().x))==2.54
assert len(gp)==9
assert len([d for d in gps.Pads() if d.GetAttribute()==p.PAD_ATTRIB_NPTH])==2
with (root/'bom.csv').open('w',newline='') as f:
    w=csv.writer(f);w.writerow(['Reference','Value','Footprint','Populate','Notes'])
    for part in design['parts']:
        notes={'J2':'Optional external LED socket; LED part unspecified','U1':'Serial #746 mounted on top with 1x9 header and two spacers','U2':'Mounted on bottom with two 1x7 headers; underside clearance required'}.get(part['ref'],'')
        w.writerow([part['ref'],part['value'],'GPS_Carrier:'+part['footprint'],'Optional' if part['dnp'] else 'Yes',notes])
manifest={f.name:hashlib.sha256(f.read_bytes()).hexdigest() for f in (root/'reference').glob('*.brd')}
(root/'reference/source-sha256.json').write_text(json.dumps(manifest,indent=2)+'\n')
result=f'PASS: {len(fps)} board components; {count} connected pins match exported schematic nets.\nPASS: Direct GPS TX/RX headers, receive-only QT monitor, LED polarity, Schottky direction and schematic paths.\nPASS: 15.24 mm QT row spacing, 2.54 mm pitches, nine GPS header pads and two GPS mounting holes.\nPCB intentionally unrouted; do not fabricate.\n'
(root/'verification.txt').write_text(result)
print(result)
