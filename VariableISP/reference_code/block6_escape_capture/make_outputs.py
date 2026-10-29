from pathlib import Path
import json,re,math
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from escape_reference import minimum_effort,fixed_exhaust,exact_shape
p=Path(__file__).parent;figdir=p/'figures';figdir.mkdir(exist_ok=True)
data=json.loads((p/'test_cases.json').read_text());c=data['case1_averaged_spirals']
plt.rcParams.update({'font.family':'DejaVu Sans','font.size':9,'axes.spines.top':False,'axes.spines.right':False})
hs=np.linspace(488,3500,180);js=np.array([minimum_effort(.5,float(h),.01,10,50)['effort'] for h in hs])
fig,ax=plt.subplots(figsize=(4.8,3.05));ax.plot(hs,js*1e4,color='#156083',lw=2,label='Finite-band reduced optimum')
ax.plot(hs,.25/hs*1e4,color='#9b642d',ls='--',label='Ideal reduced value $D^2/h$')
slow=fixed_exhaust(.5,.01,50);ax.axhline(slow['effort']*1e4,color='.45',ls=':',label='Maximum-exhaust effort floor')
ax.set(xlabel='Normalized duration $h$',ylabel='Effort $J$  ($10^{-4}$)',xlim=(400,3500),ylim=(0,5.7))
ax.grid(alpha=.15);ax.legend(fontsize=7.4,loc='upper right');fig.tight_layout()
fig.savefig(figdir/'averaged_effort.pdf');fig.savefig(figdir/'averaged_effort.png',dpi=180);plt.close(fig)
fig,ax=plt.subplots(figsize=(4.5,4.2));tt=np.linspace(0,10,501);pts=np.array([exact_shape(t)[0] for t in tt]);cap=np.array([exact_shape(10-t)[0]*np.array([1.,-1.]) for t in tt])
for path,color,label in [(pts,'#156083','Escape'),(cap,'#bc6a21','Capture')]:
 ax.plot(path[:,0],path[:,1],color=color,lw=1.8,label=label)
 for i in [180,360]:ax.annotate('',xy=path[i+12],xytext=path[i],arrowprops=dict(arrowstyle='->',color=color,lw=1.5))
ph=np.linspace(0,2*math.pi,300);ax.plot(4*np.cos(ph),4*np.sin(ph),'--',color='.4',lw=.8,label='SOI, $r=4$')
ax.plot(np.cos(ph),np.sin(ph),':',color='.4',lw=.8)
ax.fill(.8*np.cos(ph),.8*np.sin(ph),color='.86');ax.scatter([1],[0],s=22,c='black',zorder=5)
ax.set(xlabel='$x$',ylabel='$y$',aspect='equal',xlim=(-4.5,4.5),ylim=(-4.5,4.5));ax.legend(fontsize=8,loc='lower left');fig.tight_layout()
fig.savefig(figdir/'exact_escape_capture.pdf');fig.savefig(figdir/'exact_escape_capture.png',dpi=180);plt.close(fig)
t=r'''\begin{table}[tb]
\centering\small
\begin{tabular}{rl}\toprule
Duration $h$ & Reduced minimum effort $J$\\\midrule
'''
for row in c['samples']:
 t+=str(int(row['duration']))+' & '+(f"${row['effort']:.12g}$" if 'effort' in row else 'infeasible')+r'\\'+'\n'
t+=r'''\bottomrule\end{tabular}
\caption{Phase-free monotone averaged fixtures, $D=0.5$, $\chi=0.01$, exhaust band $[10,50]$. These values do not certify a full Kepler transfer.}
\label{tab:e6averaged}
\end{table}
\begin{figure}[tb]
\centering\includegraphics[width=\columnwidth]{figures/averaged_effort.pdf}
\caption{The finite exhaust ceiling produces a positive long-time effort floor in the reduced model.}
\label{fig:e6floor}
\end{figure}
\begin{figure}[tb]
\centering\includegraphics[width=\columnwidth]{figures/exact_escape_capture.pdf}
\caption{Exact feasible escape and reflected reverse capture. The dotted circle is the station orbit; the shaded disk is the excluded region $r<0.8$. These are separate local fixtures, not one optimized heliocentric route.}
\label{fig:e6exact}
\end{figure}
'''
figblocks=re.findall(r'\\begin\{figure\}.*?\\end\{figure\}',t,re.S)
t=re.sub(r'\\begin\{figure\}.*?\\end\{figure\}', '', t, flags=re.S)
(p/'block6_numbers.tex').write_text(t)
sec=p/'block6_section.tex';s=sec.read_text();s=re.sub(r'% BEGIN GENERATED NUMBERS.*?% END GENERATED NUMBERS',lambda m:'% BEGIN GENERATED NUMBERS\n'+t+'% END GENERATED NUMBERS',s,flags=re.S)
for i,fig in enumerate(figblocks,1):
 s=re.sub(r'% BEGIN FIGURE'+str(i)+r'.*?% END FIGURE'+str(i),lambda m:'% BEGIN FIGURE'+str(i)+'\n'+fig+'\n% END FIGURE'+str(i),s,flags=re.S)
sec.write_text(s)
