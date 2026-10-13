"""Lit results/*.csv|json (produits par les exécutables C++) et écrit les figures dans figures/.

Usage (depuis la racine du projet) :  python analyse/analyse.py
"""
import json
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

RACINE = Path(__file__).resolve().parent.parent
RES, FIG = RACINE / "results", RACINE / "figures"
FIG.mkdir(exist_ok=True)
DPI = 130
C_MAP, C_PLAT = "#c0504d", "#2f6fb0"      # (a) map en rouge, (b) tableau plat en bleu
C_AS, C_NAIF = "#2f6fb0", "#e08a1e"       # market maker AS / naïf
plt.rcParams.update({"font.size": 10, "axes.grid": True, "grid.alpha": 0.25, "axes.spines.top": False,
                     "axes.spines.right": False})


def sauver(fig, nom):
    fig.tight_layout()
    fig.savefig(FIG / nom, dpi=DPI)
    plt.close(fig)
    print("  ->", (FIG / nom).relative_to(RACINE))


# ------------------------------------------------------------------ carnets : latence
def figs_carnet():
    j = json.loads((RES / "bench_carnet.json").read_text(encoding="utf-8"))
    a, b = j["carnet_map"], j["carnet_plat"]

    # 1) histogrammes de latence, axes log
    fig, ax = plt.subplots(figsize=(8, 4.5))
    for d, nom, c in ((a, "(a) std::map", C_MAP), (b, "(b) tableau plat", C_PLAT)):
        h = d["histogramme"]
        bornes = np.array(h["bornes_basses_ns"])
        comptes = np.array(h["comptes"], dtype=float)
        largeurs = np.append(bornes[1:], bornes[-1] * 10 ** 0.1) - bornes
        ax.bar(bornes, comptes, width=largeurs, align="edge", alpha=0.55, color=c, label=nom, edgecolor="none")
        ax.axvline(d["global"]["p50_ns"], color=c, ls="--", lw=1.2)
        ax.axvline(d["global"]["p99_ns"], color=c, ls=":", lw=1.2)
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlim(10, 1e6)
    ax.set_xlabel("latence par opération (ns, échelle log)")
    ax.set_ylabel("nombre d'opérations (échelle log)")
    n = a["global"]["n"]
    ax.set_title(f"Latence par opération sur {n/1e6:.0f} M d'opérations (traits : p50 -- et p99 ···)")
    ax.legend()
    sauver(fig, "fig1_histogramme_latences.png")

    # 2) comparaison (a) vs (b)
    fig, axes = plt.subplots(1, 3, figsize=(13, 4.2), gridspec_kw={"width_ratios": [1.3, 1.3, 0.8]})
    ax = axes[0]
    labels = ["p50", "p99", "p99.9"]
    cles = ["p50_ns", "p99_ns", "p999_ns"]
    x = np.arange(3)
    va = [a["global"][k] for k in cles]
    vb = [b["global"][k] for k in cles]
    ax.bar(x - 0.2, va, 0.4, color=C_MAP, label="(a) map")
    ax.bar(x + 0.2, vb, 0.4, color=C_PLAT, label="(b) plat")
    for xi, (p, q) in enumerate(zip(va, vb)):
        ax.text(xi - 0.2, p * 1.1, f"{p:,.0f}", ha="center", fontsize=8)
        ax.text(xi + 0.2, q * 1.1, f"{q:,.0f}", ha="center", fontsize=8)
    ax.set_xticks(x, labels)
    ax.set_yscale("log")
    ax.set_ylim(top=max(va) * 3)
    ax.set_ylabel("ns")
    ax.set_title("Percentiles, toutes opérations")
    ax.legend()
    ax = axes[1]
    types = ["ajout", "annulation", "marche"]
    x = np.arange(3)
    va = [a[t]["p50_ns"] for t in types]
    vb = [b[t]["p50_ns"] for t in types]
    ax.bar(x - 0.2, va, 0.4, color=C_MAP)
    ax.bar(x + 0.2, vb, 0.4, color=C_PLAT)
    for xi, (p, q) in enumerate(zip(va, vb)):
        ax.text(xi - 0.2, p * 1.1, f"{p:,.0f}", ha="center", fontsize=8)
        ax.text(xi + 0.2, q * 1.1, f"{q:,.0f}", ha="center", fontsize=8)
    ax.set_xticks(x, ["ajout\n(70 %)", "annulation\n(25 %)", "ordre marché\n(5 %)"])
    ax.set_yscale("log")
    ax.set_ylim(top=max(va) * 3)
    ax.set_title("p50 par type d'opération")
    ax = axes[2]
    deb = [a["debit_ops_s"] / 1e6, b["debit_ops_s"] / 1e6]
    ax.bar([0, 1], deb, color=[C_MAP, C_PLAT])
    for xi, d in enumerate(deb):
        ax.text(xi, d * 1.02, f"{d:.1f}", ha="center")
    ax.set_xticks([0, 1], ["(a) map", "(b) plat"])
    ax.set_ylabel("Mops/s")
    ax.set_title("Débit")
    sauver(fig, "fig2_comparaison_carnets.png")


# ------------------------------------------------------------------ simulateur
def resume_sessions():
    df = pd.read_csv(RES / "sessions.csv")
    g = df.groupby(["persistance", "strategie", "L_us", "gamma", "demi_spread"])
    r = g.agg(pnl_moy=("pnl", "mean"), pnl_sd=("pnl", "std"), inv_max=("inv_max", "mean"), trades=("nb_trades", "mean"),
              spread=("spread_moyen", "mean"), n=("pnl", "size")).reset_index()
    r["pnl_se"] = r.pnl_sd / np.sqrt(r.n)
    r["ratio_moy_sd"] = np.where(r.pnl_sd > 0, r.pnl_moy / r.pnl_sd, np.nan)
    r.round(3).to_csv(RES / "resume_pnl.csv", index=False)
    return df, r


def figs_simulateur(df, r):
    meta = json.loads((RES / "simulateur_meta.json").read_text(encoding="utf-8"))
    Ls = meta["mm"]["L_us"]
    gammas = meta["mm"]["gammas"]
    regimes = sorted(df.persistance.unique())

    demis = meta["mm"]["demi_spreads_naif"]
    nom_L = [f"{L/1000:g} ms" if L >= 1000 else f"{L:g} µs" for L in Ls]
    teintes = ["#9ecae1", "#4292c6", "#08519c"]       # AS : γ croissant
    teintes_n = ["#fdd0a2", "#fd8d3c", "#a63603"]     # naïf : demi-spread croissant

    # 3) PnL vs latence, boîtes. Lignes = régime d'ordres au marché, colonnes = AS | naïf.
    fig, axes = plt.subplots(len(regimes), 2, figsize=(13, 4.4 * len(regimes)), sharex=True, squeeze=False)
    for i, reg in enumerate(regimes):
        ymin = df[df.persistance == reg].pnl.min()
        ymax = df[df.persistance == reg].pnl.max()
        for j, (strat, params, col, champ, etiq) in enumerate((("AS", gammas, teintes, "gamma", "γ = {:g}"),
                                                              ("naif", demis, teintes_n, "demi_spread", "demi-spread {:g} tick(s)"))):
            ax = axes[i, j]
            n = len(params)
            larg = 0.8 / n
            for k, (v, c) in enumerate(zip(params, col)):
                data = [df[(df.persistance == reg) & (df.strategie == strat) & (df.L_us == L) &
                           np.isclose(df[champ], v)].pnl.values for L in Ls]
                bp = ax.boxplot(data, positions=np.arange(len(Ls)) + (k - (n - 1) / 2) * larg, widths=larg * 0.9,
                                patch_artist=True, showfliers=False, medianprops=dict(color="black"))
                for box in bp["boxes"]:
                    box.set_facecolor(c)
                ax.plot([], [], "s", color=c, label=etiq.format(v))
            ax.axhline(0, color="grey", lw=0.8)
            ax.set_ylim(ymin * 1.05 - 50, ymax * 1.05)
            ax.set_xticks(range(len(Ls)), nom_L)
            ax.set_title(f"{'Avellaneda-Stoikov' if strat == 'AS' else 'Market maker naïf'}, persistance {reg:.2f}"
                         f"{' (Santa Fe pur)' if reg == 0.5 else ''}", fontsize=10)
            ax.legend(fontsize=8, loc="upper right")
            if j == 0:
                ax.set_ylabel("PnL de session (ticks·lots)")
            if i == len(regimes) - 1:
                ax.set_xlabel("latence L (période de recotation)")
    fig.suptitle(f"PnL du market maker selon la latence ({int(r.n.max())} sessions par boîte)", y=1.0)
    sauver(fig, "fig3_pnl_vs_latence.png")

    # 4) distribution du PnL AS vs naïf + plan risque/rendement
    fig, axes = plt.subplots(1, 2, figsize=(12, 4.5))
    ax = axes[0]
    L0, g0, h0 = 100, gammas[1], demis[1]
    cond = {"AS": lambda d: (d.strategie == "AS") & np.isclose(d.gamma, g0),
            "naif": lambda d: (d.strategie == "naif") & np.isclose(d.demi_spread, h0)}
    sel = df[(df.L_us == L0) & (cond["AS"](df) | cond["naif"](df))].pnl
    bins = np.linspace(sel.min(), sel.max(), 61)
    for reg, ls in ((regimes[0], "-"), (regimes[-1], "--")):
        for strat, col, nom in (("AS", C_AS, f"AS γ={g0:g}"), ("naif", C_NAIF, f"naïf h={h0:g}")):
            d = df[(df.persistance == reg) & (df.L_us == L0)]
            v = d[cond[strat](d)].pnl
            ax.hist(v, bins=bins, histtype="step", color=col, ls=ls, lw=1.6, density=True,
                    label=f"{nom}, persistance {reg:.2f} (moy {v.mean():.0f})")
    ax.set_xlabel("PnL de session (ticks·lots)")
    ax.set_ylabel("densité")
    ax.set_title(f"Distribution du PnL à L = {L0} µs")
    ax.legend(fontsize=8)
    ax = axes[1]
    sub = r[(r.persistance == regimes[0]) & (r.strategie != "sans_mm")]
    marks = {L: m for L, m in zip(Ls, "osD^")}
    for strat, col in (("AS", C_AS), ("naif", C_NAIF)):
        for L in Ls:
            s_ = sub[(sub.strategie == strat) & (sub.L_us == L)]
            ax.scatter(s_.pnl_sd, s_.pnl_moy, color=col, marker=marks[L], s=45, alpha=0.85, edgecolor="k", lw=0.4)
    ax.set_xlabel("écart-type du PnL")
    ax.set_ylabel("PnL moyen")
    ax.set_title(f"Rendement / risque, persistance {regimes[0]:.2f}")
    h = [plt.Line2D([], [], color=C_AS, marker="o", ls="", label="AS"), plt.Line2D([], [], color=C_NAIF, marker="o", ls="", label="naïf")]
    h += [plt.Line2D([], [], color="grey", marker=marks[L], ls="", label=f"L = {L:g} µs") for L in Ls]
    ax.legend(handles=h, fontsize=8)
    sauver(fig, "fig4_pnl_as_vs_naif.png")

    # 5) trajectoire d'une session exemple : session entière, zoom de 100 ms, inventaire, PnL
    t = pd.read_csv(RES / "trajectoire.csv")
    x = t.t_us / 1e6
    fig, axes = plt.subplots(4, 1, figsize=(11, 10), gridspec_kw={"height_ratios": [1.6, 1.6, 0.9, 0.9]})

    def tracer_prix(ax, m):
        xm = m.t_us / 1e6
        ax.fill_between(xm, m.meilleur_bid, m.meilleur_ask, color="grey", alpha=0.3, step="post", label="spread du carnet")
        ax.plot(xm, m.mid_zi, color="black", lw=0.9, label="mid des traders (hors MM)")
        ax.step(xm, m.mm_bid.where(m.mm_bid > 0), color=C_AS, lw=1.0, where="post", label="cote d'achat du MM")
        ax.step(xm, m.mm_ask.where(m.mm_ask > 0), color=C_NAIF, lw=1.0, where="post", label="cote de vente du MM")
        ax.set_ylabel("prix (ticks)")

    tracer_prix(axes[0], t)
    axes[0].legend(fontsize=8, ncol=2, loc="upper left")
    axes[0].set_title("Session exemple : Avellaneda-Stoikov, L = 100 µs, γ = 0.005 (Santa Fe pur)")
    z0, z1 = 1.0, 1.1
    tracer_prix(axes[1], t[(x >= z0) & (x <= z1)])
    axes[1].set_title(f"Zoom sur {z0:g} s - {z1:g} s : le MM recote autour du mid, décalé par son inventaire", fontsize=9)
    axes[0].axvspan(z0, z1, color="orange", alpha=0.2)
    axes[2].step(x, t.inventaire, color="#555", where="post")
    axes[2].axhline(0, color="grey", lw=0.8)
    axes[2].set_ylabel("inventaire (lots)")
    axes[3].plot(x, t.pnl_mtm, color="#2a8a4a")
    axes[3].axhline(0, color="grey", lw=0.8)
    axes[3].set_ylabel("PnL mark-to-market")
    axes[3].set_xlabel("temps simulé (s)")
    sauver(fig, "fig5_trajectoire.png")

    # 6) faits stylisés
    s = pd.read_csv(RES / "serie_mid.csv")
    ret = np.diff(s.mid.values)  # variation du mid sur 1 ms, en ticks
    sd = ret.std()
    z = (ret - ret.mean()) / sd
    kurt = ((z ** 4).mean() - 3)
    fig, axes = plt.subplots(1, 2, figsize=(12, 4.5))
    ax = axes[0]
    bins = np.linspace(-8, 8, 81)
    ax.hist(z, bins=bins, density=True, color=C_PLAT, alpha=0.7, label="rendements simulés (pas 1 ms)")
    xs = np.linspace(-8, 8, 400)
    ax.plot(xs, np.exp(-xs ** 2 / 2) / np.sqrt(2 * np.pi), color="k", lw=1.2, label="loi normale")
    ax.set_yscale("log")
    ax.set_ylim(1e-6, 1)
    ax.set_xlabel("variation du mid standardisée")
    ax.set_ylabel("densité (log)")
    ax.set_title(f"Queues épaisses : kurtosis en excès = {kurt:.1f}")
    ax.legend(fontsize=8)
    ax = axes[1]
    lags = np.arange(1, 201)

    def acf(v):
        v = v - v.mean()
        f = np.fft.rfft(v, 2 * len(v))
        c = np.fft.irfft(f * np.conj(f))[: len(v)]
        return (c / c[0])[lags]

    ax.vlines(lags, 0, acf(np.abs(ret)), color=C_PLAT, lw=1.2, label="|rendement|")
    ax.plot(lags, acf(ret), color=C_NAIF, lw=1.0, label="rendement")
    bande = 1.96 / np.sqrt(len(ret))
    ax.axhspan(-bande, bande, color="grey", alpha=0.2, label="IC 95 % du bruit blanc")
    ax.set_xlabel("décalage (ms)")
    ax.set_ylabel("autocorrélation")
    ax.set_title("Autocorrélation des rendements et de leur valeur absolue")
    ax.legend(fontsize=8)
    sauver(fig, "fig6_faits_stylises.png")
    # Le mid des ZI se mean-revert : la variance par seconde dépend de l'horizon (d'où le calibrage de σ² à T/2).
    var_h = {f"{h} ms": s.mid.diff(h).var() / (h * 1e-3) for h in (1, 10, 100, 1000, 2000)}
    print("  variance du mid par seconde selon l'horizon (ticks^2/s) :", {k: round(float(v), 1) for k, v in var_h.items()})
    print(f"  faits stylisés : n={len(ret)}, kurtosis excès={kurt:.2f}, acf|r|(1)={acf(np.abs(ret))[0]:.3f}, "
          f"acf|r|(10)={acf(np.abs(ret))[9]:.3f}, acf|r|(100)={acf(np.abs(ret))[99]:.3f}, acf r(1)={acf(ret)[0]:.3f}")


# ------------------------------------------------------------------ pricer
def fig_speedup():
    j = json.loads((RES / "mc_pricer.json").read_text(encoding="utf-8"))
    sp = j["speedup"]
    nt = [s["threads"] for s in sp]
    fig, ax = plt.subplots(figsize=(6.5, 4.5))
    ax.plot(nt, nt, "k--", lw=1, label="speedup idéal")
    ax.plot(nt, [s["speedup_europeen"] for s in sp], "o-", color=C_PLAT, label="call européen")
    ax.plot(nt, [s["speedup_asiatique"] for s in sp], "s-", color=C_NAIF, label="call asiatique (50 fixings)")
    ax.set_xscale("log", base=2)
    ax.set_yscale("log", base=2)
    ax.set_xticks(nt, [str(n) for n in nt])
    ax.set_yticks(nt, [str(n) for n in nt])
    ax.set_xlabel("threads")
    ax.set_ylabel("speedup vs 1 thread")
    ax.set_title(f"Pricer Monte Carlo multithread ({j['parametres']['threads_materiels']} threads matériels)")
    ax.legend()
    sauver(fig, "fig7_speedup_pricer.png")


if __name__ == "__main__":
    print("figures :")
    figs_carnet()
    df, r = resume_sessions()
    figs_simulateur(df, r)
    fig_speedup()
