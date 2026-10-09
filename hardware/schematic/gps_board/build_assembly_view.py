"""Engineering assembly illustration for the serial-GPS opposite-face carrier."""
from pathlib import Path
from html import escape
import json
root=Path(__file__).resolve().parent
pm=json.loads((root/'reference/pad-map.json').read_text())
s=['<svg xmlns="http://www.w3.org/2000/svg" width="1120" height="980" viewBox="0 0 1120 980">','<rect width="1120" height="980" fill="#f4f6f8"/>','<style>text{font-family:Arial,sans-serif;fill:#172a3a}</style>']
def text(x,y,t,size=16,fill=None):
 s.append(f'<text x="{x}" y="{y}" font-size="{size}"'+(f' style="fill:{fill}"' if fill else '')+'>'+escape(t)+'</text>')
def rect(x,y,w,h,color,stroke='#243746',r=0):
 s.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{r}" fill="{color}" stroke="{stroke}" stroke-width="1.5"/>')
def circ(x,y,r,fill,stroke='#243746'):
 s.append(f'<circle cx="{x}" cy="{y}" r="{r}" fill="{fill}" stroke="{stroke}" stroke-width="1.5"/>')
def line(x1,y1,x2,y2):s.append(f'<path d="M{x1} {y1}L{x2} {y2}" stroke="#516171" stroke-width="1.5" fill="none"/>')
def coord(x,y,side):
 if side=='bottom':x=200-x
 return (115 if side=='top' else 715)+(x-86.8)*10,135+(y-82.355)*10

def boxmm(x,y,w,h,side,fill):
 if side=='bottom':x=200-x-w
 xx=(115 if side=='top' else 715)+(x-86.8)*10; yy=135+(y-82.355)*10
 rect(xx,yy,w*10,h*10,fill,r=5)
text(38,44,'Serial GPS / QT Py — mounted on opposite faces',28)
text(38,74,'Revision B  |  26.40 × 35.29 mm carrier  |  Direct header connections',17)
text(38,116,'TOP — complete GPS breakout #746',18)
text(638,116,'BOTTOM — viewed from underneath',18)
for side in ['top','bottom']:boxmm(86.8,82.355,26.4,35.29,side,'#d6e9dc')
boxmm(87.3,82.855,25.4,34.29,'top','#7c5aa6')
boxmm(90.6,92,18,18,'top','#e8d9b8')
text(153,213,'Ultimate GPS',20,fill='white')
text(163,326,'GPS antenna',18)
gps=pm['Adafruit_Ultimate_GPS_Serial_746_THT']
for side in ['top','bottom']:
 for hole in gps['holes']:
  x,y=coord(100+hole['x'],100+hole['y'],side);circ(x,y,13,'#f4f6f8','#d8bb69')
 for pad in gps['pads']:
  x,y=coord(100+pad['x'],100+pad['y'],side);circ(x,y,8.5,'#d8bb69');circ(x,y,3.5,'#f4f6f8')
boxmm(88.1495,90.11,20.701,17.78,'bottom','#7c5aa6')
boxmm(87.05,94.5,7.55,9,'bottom','#d6dbe0')
for pad in pm['Adafruit_QT_Py_ESP32_S2_THT']['pads']:
 x,y=coord(98.5+pad['y'],99+pad['x'],'bottom');circ(x,y,8,'#d8bb69');circ(x,y,3.5,'#f4f6f8')
text(789,301,'QT Py',21,fill='white')
text(783,324,'ESP32-S2',14,fill='white')
boxmm(90.55,108.9,7.5,3.9,'bottom','#f7f4e8')
boxmm(103.06,109.1,5.34,2.8,'bottom','#34414b')
text(980,393,'PicoBlade',14);line(977,389,940,422)
text(640,416,'LED socket',14);line(710,411,775,419)
text(969,282,'USB-C',14);text(969,302,'programming',12);line(971,310,945,316)
text(38,524,'Two mounting holes + a 9-pin header mount the GPS.',16)
text(38,548,'VIN, GND, TX and RX connect through that header.',16)
text(638,524,'Two 7-pin headers mount the QT Py on the other face.',16)
text(638,548,'PicoBlade and optional LED socket face outward.',16)
line(38,573,1082,573)
text(38,610,'SIDE VIEW — vertical gaps illustrative; select spacers after fit check',19)
rect(130,640,310,23,'#7c5aa6');text(550,658,'Complete GPS board — antenna faces up',17)
rect(150,663,24,38,'#d8bb69');rect(400,663,24,38,'#a8b3bd')
text(550,689,'Board-to-board header and spacers',16)
rect(112,701,346,13,'#699c79');text(550,716,'Carrier PCB — 1.6 mm nominal',17)
rect(175,714,22,32,'#d8bb69');rect(375,714,22,32,'#d8bb69')
rect(166,746,242,23,'#7c5aa6');text(550,765,'QT Py — components face down',17)
text(38,818,'Electrical path',19)
text(38,849,'Main-nav TX → GPS RX. GPS TX → main-nav RX and QT Py RX (through 4.7 kohm).',17)
text(38,876,'QT Py TX is unconnected. GPIO18 controls the optional status LED.',17)
text(38,915,'No flying-wire taps. No GPS USB connector. The PCB is still unrouted.',16)
text(38,951,'CAD-based module/header positions; illustrative component shapes and heights. See README for print/CAD discrepancy.',13)
s.append('</svg>')
(root/'preview/assembly.svg').write_text('\n'.join(s),encoding='utf-8')
print('Created serial-GPS preview/assembly.svg')
