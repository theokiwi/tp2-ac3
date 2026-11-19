#!/usr/bin/env python3
import pandas as pd
import os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CSV = os.path.join(ROOT, "sim", "results", "battery.csv")
df = pd.read_csv(CSV, engine="python")

pd.set_option("display.width", 160)
pd.set_option("display.max_rows", 200)

print("===== linhas totais =====")
print(len(df))
print("workloads:", sorted(df.workload.unique()))
print("policies:", sorted(df.policy.unique()))
print("ram_fractions:", sorted(df.ram_fraction.unique()))
print("period_density:", sorted(df.period_density.unique()))

print("\n===== H1: aging vs chunkq por densidade, RAM=50%, media+-std, n =====")
sub = df[(df.ram_fraction == 0.5) & (df.policy.isin(["chunkq", "aging"]))]
g = sub.groupby(["period_density", "policy"])["page_ins"].agg(["mean", "std", "count"])
print(g)
# diferenca relativa
for dens in ["sparso", "medio", "denso"]:
    a = sub[(sub.period_density == dens) & (sub.policy == "aging")]["page_ins"].mean()
    c = sub[(sub.period_density == dens) & (sub.policy == "chunkq")]["page_ins"].mean()
    print(f"{dens}: aging={a:.1f} chunkq={c:.1f} diff={(c-a):.1f} diff%={100*(c-a)/a:.2f}%")

print("\n===== H1 por workload individual (RAM=50%, denso) =====")
sub1b = df[(df.ram_fraction == 0.5) & (df.period_density == "denso") & (df.policy.isin(["chunkq", "aging"]))]
print(sub1b.groupby(["workload", "policy"])["page_ins"].agg(["mean", "count"]))

print("\n===== H2: todas politicas x ram_fraction (media page_ins, todas cargas) =====")
g2 = df.groupby(["ram_fraction", "policy"])["page_ins"].agg(["mean", "std"])
print(g2)
print("\nnormalizado por OPT (quanto cada politica eh pior que OPT, em x):")
opt_means = df[df.policy == "opt"].groupby("ram_fraction")["page_ins"].mean()
for f in sorted(df.ram_fraction.unique()):
    print(f"-- RAM {int(f*100)}% -- OPT={opt_means[f]:.1f}")
    for pol in ["lru", "aging", "chunkq", "clock"]:
        m = df[(df.ram_fraction == f) & (df.policy == pol)]["page_ins"].mean()
        print(f"   {pol}: {m:.1f}  ({m/opt_means[f]:.2f}x OPT)")

print("\ngap relativo aging-chunkq por fracao de RAM:")
for f in sorted(df.ram_fraction.unique()):
    a = df[(df.ram_fraction == f) & (df.policy == "aging")]["page_ins"].mean()
    c = df[(df.ram_fraction == f) & (df.policy == "chunkq")]["page_ins"].mean()
    print(f"RAM {int(f*100)}%: aging={a:.1f} chunkq={c:.1f} diff={(c-a):.1f} diff%={100*(c-a)/a:.2f}%")

print("\n===== H3: sequencial ciclica, hit_rate por politica x ram_fraction =====")
s3 = df[df.workload == "sequential"].copy()
s3["hit_rate"] = 100 * (1 - s3.page_ins / s3.post_accesses)
print(s3[["ram_fraction", "policy", "page_ins", "post_accesses", "hit_rate"]].sort_values(["ram_fraction", "policy"]))

print("\n===== lru_agreement e split_precision medios por politica =====")
print(df.groupby("policy")[["split_precision", "lru_agreement", "decision_time_ms"]].mean())

print("\n===== decision_time_ms medio por politica (ordenado) =====")
print(df.groupby("policy")["decision_time_ms"].mean().sort_values())
