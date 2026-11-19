#!/usr/bin/env python3
"""Gera fig_h1.png, fig_h2.png, fig_h3.png a partir de sim/results/battery.csv.
Uso: bash sim/scripts/pyenv.sh sim/scripts/make_figs.py
"""
import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CSV = os.path.join(ROOT, "sim", "results", "battery.csv")
FIGDIR_SIM = os.path.join(ROOT, "sim", "figs")
FIGDIR_ARTICLE = os.path.join(ROOT, "article", "figs")
FIGDIR_BANNER = os.path.join(ROOT, "banner")
os.makedirs(FIGDIR_SIM, exist_ok=True)
os.makedirs(FIGDIR_ARTICLE, exist_ok=True)

df = pd.read_csv(CSV, engine="python")

C_CHUNKQ = "#2a78d6"
C_AGING = "#6b7280"
C_LRU = "#1baf7a"
C_OPT = "#eda100"
C_CLOCK = "#e34948"
POLICY_COLOR = {"chunkq": C_CHUNKQ, "aging": C_AGING, "lru": C_LRU, "opt": C_OPT, "clock": C_CLOCK}
POLICY_LABEL = {"chunkq": "chunk queues", "aging": "aging 8-bit", "lru": "LRU exato", "opt": "OPT", "clock": "CLOCK"}

plt.rcParams.update({
    "font.size": 11,
    "axes.edgecolor": "#888888",
    "axes.labelcolor": "#222222",
    "text.color": "#222222",
    "xtick.color": "#444444",
    "ytick.color": "#444444",
    "axes.grid": True,
    "grid.color": "#e5e5e5",
    "grid.linewidth": 0.6,
    "axes.axisbelow": True,
    "figure.facecolor": "white",
    "axes.facecolor": "white",
})

def save(fig, name):
    for d in (FIGDIR_SIM, FIGDIR_ARTICLE, FIGDIR_BANNER):
        fig.savefig(os.path.join(d, name), dpi=170, bbox_inches="tight")

# ---------- H1: chunk queues vs aging, page_ins por densidade de periodo ----------
# media sobre todas as cargas aleatorias e sementes, RAM=50%, normalizado por N_blocks
sub = df[(df.ram_fraction == 0.5) & (df.policy.isin(["chunkq", "aging"]))]
order = ["sparso", "medio", "denso"]
agg = sub.groupby(["period_density", "policy"])["page_ins"].agg(["mean", "std"]).reset_index()

fig, ax = plt.subplots(figsize=(5.0, 3.6))
width = 0.35
x = range(len(order))
for i, pol in enumerate(["aging", "chunkq"]):
    means, stds = [], []
    for pd_ in order:
        row = agg[(agg.period_density == pd_) & (agg.policy == pol)]
        means.append(row["mean"].values[0] if len(row) else 0)
        stds.append(row["std"].values[0] if len(row) else 0)
    xs = [xi + (i - 0.5) * width for xi in x]
    ax.bar(xs, means, width=width, yerr=stds, capsize=3, color=POLICY_COLOR[pol],
           label=POLICY_LABEL[pol])
ax.set_xticks(list(x))
ax.set_xticklabels(["esparso\n(~1,2%)", "medio\n(~12%)", "denso\n(~55%)"])
ax.set_ylabel("page-ins pos-migracao (media)")
ax.set_title("H1 - aging vs chunk queues por densidade de acessos/periodo")
ax.legend(frameon=False)
fig.tight_layout()
save(fig, "fig_h1.png")
plt.close(fig)

# ---------- H2: diferenca entre politicas nas 3 fracoes de RAM ----------
sub2 = df.copy()
agg2 = sub2.groupby(["ram_fraction", "policy"])["page_ins"].mean().reset_index()
fracs = sorted(df.ram_fraction.unique())
policies = ["opt", "lru", "aging", "chunkq", "clock"]

fig, ax = plt.subplots(figsize=(5.4, 3.6))
width = 0.15
x = range(len(fracs))
for i, pol in enumerate(policies):
    vals = []
    for f in fracs:
        row = agg2[(agg2.ram_fraction == f) & (agg2.policy == pol)]
        vals.append(row["page_ins"].values[0] if len(row) else 0)
    xs = [xi + (i - 2) * width for xi in x]
    ax.bar(xs, vals, width=width, color=POLICY_COLOR[pol], label=POLICY_LABEL[pol])
ax.set_xticks(list(x))
ax.set_xticklabels([f"{int(f*100)}%" for f in fracs])
ax.set_xlabel("fracao de RAM")
ax.set_ylabel("page-ins pos-migracao (media)")
ax.set_title("H2 - todas as politicas por fracao de RAM")
ax.legend(frameon=False, fontsize=8, ncol=3)
fig.tight_layout()
save(fig, "fig_h2.png")
plt.close(fig)

# ---------- H3: sequencial ciclica, todas as politicas, por fracao de RAM ----------
sub3 = df[df.workload == "sequential"].copy()
sub3["hit_rate"] = 1 - sub3.page_ins / sub3.post_accesses
agg3 = sub3.groupby(["ram_fraction", "policy"])["hit_rate"].mean().reset_index()

fig, ax = plt.subplots(figsize=(5.0, 3.6))
width = 0.15
x = range(len(fracs))
for i, pol in enumerate(policies):
    vals = []
    for f in fracs:
        row = agg3[(agg3.ram_fraction == f) & (agg3.policy == pol)]
        vals.append(100 * row["hit_rate"].values[0] if len(row) else 0)
    xs = [xi + (i - 2) * width for xi in x]
    ax.bar(xs, vals, width=width, color=POLICY_COLOR[pol], label=POLICY_LABEL[pol])
ax.set_xticks(list(x))
ax.set_xticklabels([f"{int(f*100)}%" for f in fracs])
ax.set_xlabel("fracao de RAM")
ax.set_ylabel("taxa de acerto (%)")
ax.set_title("H3 - sequencial ciclica (VM > RAM)")
ax.legend(frameon=False, fontsize=8, ncol=3)
fig.tight_layout()
save(fig, "fig_h3.png")
plt.close(fig)

print("figuras geradas em:", FIGDIR_SIM, FIGDIR_ARTICLE, FIGDIR_BANNER)
