# Compact GPS carrier — revision B, serial GPS

Open **gps_carrier.kicad_pro** in KiCad 9. The complete Adafruit Ultimate GPS
serial breakout **#746** mounts on the **top** face. The complete QT Py
ESP32-S2 **#5325** mounts on the **bottom** face. The PicoBlade and optional
LED socket are on the QT Py face.

This revision uses `correct_serial_gpsprint.png`. It replaces the USB GPS,
removes tap pads J3, jumper SJ1 and unnecessary diode D2. GPS power and UART
connect through a **nine-pin board-to-board header**. No GPS USB connection,
flying-wire taps or modifications to the GPS module are required.

## Files

- `gps_carrier.kicad_sch`: editable schematic.
- `gps_carrier.kicad_pcb`: **unrouted**, compact opposite-face placement.
- `GPS_Carrier.kicad_sym`, `GPS_Carrier.pretty/`: local libraries.
- `preview/gps_carrier.pdf`: schematic.
- `preview/assembly.png` / `.svg`: top, bottom and side assembly views.
- `preview/placement.pdf`: actual KiCad placement drawing.
- `bom.csv`, `erc.rpt`, `placement-drc.rpt`, `verification.txt`: checks/parts.
- `gps_carrier_rev_A.zip`: superseded historical USB-GPS design.

## Connections

| Connector | Pin | Function |
|---|---:|---|
| J1 Molex 53047-0410 | 1 | Regulated +5 V from main nav |
| J1 | 2 | Ground |
| J1 | 3 | Main-nav TX -> GPS RX |
| J1 | 4 | Main-nav RX <- GPS TX |
| J2 optional 2.54 mm socket | 1 | LED anode (+), through R2 |
| J2 | 2 | LED cathode (-), switched by Q1 |

**J1 pin order remains an assumption**, following the supplied 5V/GND/TX/RX
list; verify against the actual main-nav cable before fabrication. Logic
is 3.3 V; this is not an RS-232 interface.

GPS TX connects directly to J1.4 and, through R1 (4.7 kohm), to QT Py
RX / GPIO16. QT Py TX is unconnected. Main-nav TX connects directly to GPS
RX. QT Py only receives GPS data and controls the indicator.

GPS symbol/footprint numbering follows Adafruit JP1. Viewed from above with
the antenna up and header at the bottom, **pin 1 is the rightmost pin**.

| GPS U1 pin | Printed signal | Carrier connection |
|---:|---|---|
| 1 | PPS | Unconnected |
| 2 | VIN | +5V_NAV |
| 3 | GND | GND |
| 4 | RX | NAV_TX |
| 5 | TX | GPS_TX: nav RX and QT monitor |
| 6 | FIX | Unconnected; firmware evaluates serial fix data |
| 7 | VBAT | Unconnected; use GPS onboard backup-cell holder |
| 8 | EN | Unconnected; GPS onboard pull-up enables receiver |
| 9 | 3.3V | Unconnected regulator output |

## Indicator and power

QT Py A0 / GPIO18 drives Q1 (MMBT3904: 1=B, 2=E, 3=C) through R3 (2.2 kohm).
R4 (100 kohm) holds the LED off during reset. HIGH turns it on. R2 (220 ohm)
limits LED current to approximately 6–14 mA, depending on forward voltage
and supply/diode drop. Choose and test a high-efficiency LED for the desired
visibility; this is for one indicator LED, not a power lamp. J2 is a generic
1x2 female header at 2.54 mm pitch, marked DNP until fitted. No external LED
part number has been selected.

GPS VIN connects directly to NAV 5 V. D1 (SS14) feeds QT Py 5 V and prevents
QT USB power returning to NAV through the power line. QT USB-C remains for
programming. Keep its onboard USB protection jumper in its factory state.
Do not connect the modules' 3.3 V regulator outputs together.

D1 does not isolate UART paths. Power the system together while signals are
active to avoid back-powering an unpowered input. R1 limits monitor-branch
current; it is not a level shifter. Firmware should keep RX as an input,
leave TX unused, disable unnecessary Wi-Fi, and detect stale/missing NMEA
data as well as fix quality. Firmware is not included.

## Mechanical design

- Carrier: **26.400 x 35.290 mm**, two copper layers, nominal 1.6 mm thick.
- GPS: **25.400 x 34.290 mm** CAD envelope, antenna outward, mounted above
  the carrier using a **1x9 header, 2.54 mm pitch**, and two spacers.
- GPS pad centers: X = +10.16 to -10.16 mm (pins 1 to 9), Y = +15.113 mm,
  relative to the centered footprint origin. Header drills: 1.0 mm.
- GPS mounting centers: X = -10.160 / +10.160 mm, Y = -14.605 mm;
  two **2.5 mm NPTH** carrier holes. Select compatible small screws/spacers.
- QT Py: **17.780 x 20.701 mm** PCB envelope, **15.240 mm** between header
  rows, **2.540 mm** pitch. Two 1x7 headers with 1.0 mm carrier drills.
  It is rotated so programming USB faces the carrier's side.
- Supplied QT Py overall size: 21.8 x 17.9 x 5.7 mm. Connector overhang and
  installed cables extend beyond bare PCB envelopes.

**Print/CAD discrepancy:** the supplied GPS print labels height 1.34 in,
bottom-edge-to-mounting-hole distance 1.24 in, and holes 0.09 in. Original
Adafruit v3 CAD gives 1.35 in, 1.25 in and 2.5 mm respectively. This design
uses exact CAD coordinates. Check the actual module before manufacture,
particularly the **0.254 mm vertical mounting difference**. The image alone
does not establish which set matches the physical module. Header pitch,
width and signal order agree.

The ordinary GPS footprint has a full module courtyard. The `_Raised`
variant reserves header/spacer positions while showing the body envelope
on User.Drawings. This permits SMD parts underneath; it does **not** prove
vertical clearance. Select headers/spacers after measuring the GPS battery
holder and installed cell, QT Py underside components, carrier components
and solder joints. Do not mount either module flush. Final stack height and
header/spacer part numbers remain unspecified. Check USB plug, PicoBlade
cable, LED socket and screw-head clearance on the assembled stack.

Assembly-view vertical spacing is illustrative only. There are no module
3D models. GPS antenna faces outward on top. Test acquisition/reception in
the final enclosure with the QT Py and LED operating; RF performance has
not been validated.

## Validation and scope

KiCad 9 parses the schematic, libraries and board. ERC: **zero errors or
warnings**. Placement DRC: **zero placement violations**, with **17 expected
unrouted connections**. `verify_design.py` checks actual PCB pad nets against
KiCad's exported schematic netlist, direct UART connections, QT receive-only
behavior, LED polarity, diode orientation, pitches and schematic association.

The PCB remains **unrouted**, with no fabrication outputs. Before ordering,
verify physical module dimensions and cable pin order, choose mounting
hardware/LED, finish routing and run full DRC.

## Sources and regeneration

Reference CAD, original attribution and licenses are in `reference/`.
`pad-map.json` records source positions; `source-sha256.json` records hashes.

- [Adafruit Ultimate GPS CAD](https://github.com/adafruit/Adafruit-Ultimate-GPS):
  `Adafruit Ultimate GPS.brd` -> `reference/gps-serial.brd`.
- [GPS pinouts](https://learn.adafruit.com/adafruit-ultimate-gps/pinouts).
- [QT Py ESP32-S2 CAD](https://github.com/adafruit/Adafruit-QT-Py-ESP32-S2-PCB):
  `Adafruit QT Py ESP32-S2.brd` -> `reference/qtpy-s2.brd`.
- [QT Py pinouts](https://learn.adafruit.com/adafruit-qt-py-esp32-s2/pinouts).

Adafruit designs: Limor Fried/Ladyada for Adafruit Industries. Preserve
original attribution and Creative Commons Share-Alike terms supplied in
reference files when redistributing derivative geometry. Standard KiCad
components retain their CC-BY-SA 4.0 library-exception terms and standard
3D model links. Historical USB CAD is retained only for provenance.

Regenerate with `build_libraries.py`, `build_schematic.py`, `build_pcb.py`
and `build_assembly_view.py`. Use KiCad's bundled Python for `build_pcb.py`
and `verify_design.py`. These overwrite generated files; preserve manual
changes first.
