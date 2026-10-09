"""KiCad pcbnew placement study. Unrouted by intent; not fabrication output.

Run using the Python supplied with KiCad 9.
"""
from pathlib import Path
import json
import pcbnew as p

ROOT=Path(__file__).resolve().parent
data=json.loads((ROOT/'reference/design.json').read_text())
b=p.BOARD()
b.SetCopperLayerCount(2)
b.GetDesignSettings().SetBoardThickness(p.FromMM(1.6))
def v(x,y): return p.VECTOR2I(p.FromMM(x),p.FromMM(y))
nets={}
for part in data['parts']:
    for name in part['nets'].values():
        if name not in nets:
            net=p.NETINFO_ITEM(b,'/'+name,len(nets)+1); b.Add(net); nets[name]=net
# QT Py on the underside; GPS raised above the top surface.
positions={
 'U1':(100,100,0,False),'U2':(98.5,99,270,True),
 'J1':(96.2,111.1,180,True),'J2':(107,110.5,90,True),
 'D1':(100,86,0,False),
 'R1':(101,112,0,False),'R2':(110,108,0,False),
 'R3':(105,99,0,False),'R4':(105,102,0,False),
 'Q1':(110,100,0,False)}
for part in data['parts']:
    fp=p.FootprintLoad(str(ROOT/'GPS_Carrier.pretty'),part['footprint'])
    b.Add(fp)
    fp.SetFPID(p.LIB_ID('GPS_Carrier',part['footprint']))
    fp.SetReference(part['ref']); fp.SetValue(part['value'])
    path=p.KIID_PATH(); path.push_back(p.KIID(data['root_uuid'])); path.push_back(p.KIID(part['uuid'])); fp.SetPath(path)
    x,y,a,back=positions[part['ref']]
    fp.SetPosition(v(x,y))
    if back: fp.Flip(v(x,y),p.FLIP_DIRECTION_LEFT_RIGHT)
    fp.SetOrientationDegrees(a)
    fp.SetDNP(part['dnp'])
    for pad in fp.Pads():
        n=part['nets'].get(pad.GetNumber())
        if n: pad.SetNet(nets[n])
    fp.Value().SetVisible(False)
    fp.Reference().SetTextSize(v(.8,.8)); fp.Reference().SetTextThickness(p.FromMM(.12))
    if part['ref']=='J1': fp.Reference().SetPosition(v(97.5,113.4))
    if part['ref']=='U1': fp.Reference().SetPosition(v(100,109))
    if part['ref']=='U2':
        fp.Reference().SetPosition(v(99,99))
        print('QT Py pads:',[(pad.GetNumber(),p.ToMM(pad.GetPosition().x),p.ToMM(pad.GetPosition().y)) for pad in fp.Pads()])
x1,y1,x2,y2=86.8,82.355,113.2,117.645
for start,end in [((x1,y1),(x2,y1)),((x2,y1),(x2,y2)),((x2,y2),(x1,y2)),((x1,y2),(x1,y1))]:
    line=p.PCB_SHAPE(); line.SetShape(p.SHAPE_T_SEGMENT); line.SetStart(v(*start));line.SetEnd(v(*end)); line.SetLayer(p.Edge_Cuts);line.SetWidth(p.FromMM(.05));b.Add(line)
for text,x,y,size in [('TWO-SIDED MODULE CARRIER - UNROUTED',100,76,1.2),('TOP: serial GPS   /   BOTTOM: QT Py',100,78.5,1),('26.40 x 35.29 mm; GPS 9-pin header + 2 mounting holes',100,121,.9),('Direct UART through headers; no flying wires',100,123,.9)]:
    t=p.PCB_TEXT(b);t.SetText(text);t.SetPosition(v(x,y));t.SetTextSize(v(size,size));t.SetTextThickness(p.FromMM(.15));t.SetLayer(p.Dwgs_User);b.Add(t)
p.SaveBoard(str(ROOT/'gps_carrier.kicad_pcb'),b)
print('Saved unrouted 26.40 x 35.29 mm serial-GPS placement study')
