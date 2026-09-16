import csv, numpy as np, math
D="/Users/djmcmath/Documents/tern/dpv-nav/baseline cal jsons/thermal testing/"
runs={"north":0,"east":90,"south":180,"west1":270,"west2":270}
BH=17.0
out={}
for r,psi in runs.items():
    rows=list(csv.DictReader(open(D+r+".csv")))
    t=np.array([float(x["mag_temp_c"]) for x in rows]); ts=np.array([float(x["timestamp_ms"]) for x in rows])/1000
    ip=int(np.argmax(t)); m=ts>ts[ip]+30
    X=np.c_[np.ones(m.sum()),t[m]]
    s=[np.linalg.lstsq(X,np.array([float(x["mag_%s_cal"%a]) for x in rows])[m],rcond=None)[0][1] for a in "xy"]
    sh=math.hypot(*s); ph=math.degrees(math.atan2(s[1],s[0]))
    err=math.degrees(sh/BH)*math.sin(math.radians(ph-psi))
    print(f"{r:6s} sx={s[0]:.3f} sy={s[1]:.3f} |sh|={sh:.3f} phase={ph:.1f} err@{psi}={err:+.2f} deg/C  Trange={t.min():.1f}-{t.max():.1f}")
    out[r]=(psi,err)

import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt
BG="#111a24"; LINE="#1e2b38"; INK="#f1f5f9"; BODY="#b3c0cd"; MUTED="#7d8b99"; TAN="#c4beae"
POS="#e0a458"; NEG="#5fa8d3"
sx,sy=0.501,0.421; A=math.degrees(math.hypot(sx,sy)/BH); PH=math.degrees(math.atan2(sy,sx))
OFF=4.0
plt.rcParams.update({"font.family":["Inter","Helvetica Neue","Arial","DejaVu Sans"]})
fig=plt.figure(figsize=(8,8.4),dpi=200,facecolor=BG)
ax=fig.add_axes([0.1,0.08,0.8,0.8],projection="polar",facecolor=BG)
ax.set_theta_zero_location("N"); ax.set_theta_direction(-1)
th=np.radians(np.arange(0,360.5,0.5)); e=A*np.sin(np.radians(PH)-th)
ax.fill_between(th,OFF,OFF+np.maximum(e,0),color=POS,alpha=.28,lw=0)
ax.fill_between(th,OFF+np.minimum(e,0),OFF,color=NEG,alpha=.28,lw=0)
ax.plot(th,np.full_like(th,OFF),color=MUTED,lw=1.2,ls=(0,(4,3)))
ax.plot(th,OFF+e,color=TAN,lw=2.2)
ax.set_ylim(0,OFF+2.8)
ax.set_rticks([OFF-2,OFF-1,OFF,OFF+1,OFF+2])
ax.set_yticklabels(["−2°","−1°","0°","+1°","+2°"],color=MUTED,fontsize=9)
ax.set_rlabel_position(67.5)
ax.set_xticks(np.radians(np.arange(0,360,45)))
ax.set_xticklabels(["N","NE","E","SE","S","SW","W","NW"],color=BODY,fontsize=13,fontweight="bold")
ax.tick_params(axis="x",pad=10)
ax.grid(color=LINE,lw=1); ax.spines["polar"].set_color(LINE)
# nulls and peaks
for hd,lab in [(PH,"null\n%03d°"%round(PH)),(PH+180,"null\n%03d°"%round(PH+180))]:
    ax.plot(np.radians(hd),OFF,"o",ms=7,mfc=BG,mec=TAN,mew=1.6,zorder=5)
for hd,val in [(PH+90,-A),(PH+270,A)]:
    ax.plot(np.radians(hd),OFF+val,"o",ms=5,color=TAN,zorder=5)
    ax.annotate(("worst: %+.1f°/°C at %03d°"%(val,round(hd%360))).replace("-","−"),(np.radians(hd),OFF+val),
        xytext=(np.radians(hd),OFF+val+(-1.1 if val>0 else 3.0)),ha="center",va="center",color=INK,fontsize=10)
ax.annotate("null %03d°"%round(PH),(np.radians(PH),OFF),xytext=(np.radians(PH+14),OFF+0.55),color=BODY,fontsize=10,ha="center")
ax.annotate("null %03d°"%round(PH+180),(np.radians(PH+180),OFF),xytext=(np.radians(PH+180+12),OFF+1.05),color=BODY,fontsize=10,ha="center")
# measured runs (west1/west2 averaged into one marker, both shown)
lbl={"north":"N run","east":"E run","south":"S run","west1":"W runs (×2)"}
for r,(psi,err) in out.items():
    ax.plot(np.radians(psi),OFF+err,"o",ms=10,color=INK,mec=BG,mew=2,zorder=6)
for r,(psi,err) in out.items():
    if r=="west2": continue
    dx={0:-11,90:0,180:22,270:0}[psi]; dr={0:1.1,90:-1.25,180:-0.2,270:-1.3}[psi]
    ax.annotate(f"{lbl[r]}\n{err:+.1f}°/°C".replace("-","−"),(np.radians(psi),OFF+err),xytext=(np.radians(psi+dx),OFF+err+dr),
        color=INK,fontsize=10,ha="center",va="center",fontweight="bold")
# Friday's single-heading test
f=16; fe=A*math.sin(math.radians(PH-f))
ax.plot(np.radians(f),OFF+fe,"D",ms=8,mfc=BG,mec=POS,mew=2,zorder=6)
ax.annotate("Friday, 016°\n%+.1f°/°C"%fe,(np.radians(f),OFF+fe),xytext=(np.radians(f+20),OFF+fe+0.55),color=POS,fontsize=10,ha="center")
fig.text(0.5,0.955,"Heading error per °C of die warming, by heading",ha="center",color=INK,fontsize=15,fontweight="bold")
fig.text(0.5,0.925,"Dashed ring = no error. Outside: compass reads high (+). Inside: reads low (−).",ha="center",color=BODY,fontsize=10.5)
fig.text(0.5,0.03,"Curve: pooled drift (0.50, 0.42) µT/°C, centred cal, 17 µT horizontal field.  Dots: each heat-gun run's own drift at its heading.",
    ha="center",color=MUTED,fontsize=8.5)
fig.savefig("polar.png",facecolor=BG)
