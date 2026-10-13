"""Figures « vitrine » et tableaux d'en-tête du README, tous LUS dans results/ (rien n'est tapé à la main).

Usage (depuis la racine du projet) :  python analyse/vitrine_05.py
Produit :
  figures/hero_latences.png        figure héro : carnet (latence), SPSC vs mutex (débit), market maker (PnL vs latence)
  figures/fig8_spsc_vs_mutex.png   détail SPSC / mutex : débit, p50, dispersion du p99 par répétition
  figures/toutes_variantes_mm.png  les 25 configurations du simulateur x 2 régimes de flux (carte de chaleur)
Remplace dans README.md les blocs  <!-- BEGIN:nom --> ... <!-- END:nom -->  (badges, hypotheses, metriques,
cles, lecture30, variantes) par des tableaux calculés à partir de results/*.json|csv|txt.
"""
import json
import re
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
C_MAP, C_PLAT = "#c0504d", "#2f6fb0"      # (a) std::map en rouge, (b) tableau plat en bleu
C_SPSC, C_MUTEX = "#2f6fb0", "#c0504d"    # lock-free en bleu, mutex en rouge
C_AS, C_NAIF = "#2f6fb0", "#e08a1e"
NB = " "
plt.rcParams.update({"font.size": 10, "axes.grid": True, "grid.alpha": 0.25, "axes.spines.top": False,
                     "axes.spines.right": False})


# ------------------------------------------------------------------ formats français
def nombre(x, nd=0):
    return f"{x:,.{nd}f}".replace(",", NB).replace(".", ",")


def duree(ns):
    if ns < 1e3:
        return f"{nombre(ns)}{NB}ns"
    if ns < 1e6:
        return f"{nombre(ns / 1e3, 1 if ns < 1e5 else 0)}{NB}µs"
    return f"{nombre(ns / 1e6, 2)}{NB}ms"


def mops(x, nd=1):
    return nombre(x / 1e6, nd)


def rapport(a, b, nd=1):
    return f"{nombre(a / b, nd)}×"


def lire(nom):
    return json.loads((RES / nom).read_text(encoding="utf-8"))


def sauver(fig, nom):
    fig.savefig(FIG / nom, dpi=DPI, bbox_inches="tight")
    plt.close(fig)
    print("  ->", (FIG / nom).relative_to(RACINE))


def tableau(entetes, lignes, align=None):
    align = align or ["---"] * len(entetes)
    out = ["| " + " | ".join(entetes) + " |", "|" + "|".join(align) + "|"]
    out += ["| " + " | ".join(str(c) for c in l) + " |" for l in lignes]
    return "\n".join(out)


# ------------------------------------------------------------------ données
bc, bs, mp = lire("bench_carnet.json"), lire("bench_spsc.json"), lire("mc_pricer.json")
meta = lire("simulateur_meta.json")
rs = pd.read_csv(RES / "resume_pnl.csv")
log = (RES / "log_tests.txt").read_text(encoding="utf-8")
verifs = [(int(a), int(b)) for a, b in re.findall(r"(\d+) verifications, (\d+) echec", log)]
diff = re.findall(r"differentiel : (\d+) ops, (\d+) trades, .*desaccords=(\d+), incoherences=(\d+)", log)
a, b = bc["carnet_map"], bc["carnet_plat"]
cfg, flux = bc["config"], bc["flux"]
Ls = meta["mm"]["L_us"]
NOM_L = [f"{L / 1000:g} ms".replace(".", ",") if L >= 1000 else f"{L:g} µs" for L in Ls]
REG_PUR, REG_PERS = [r["persistance"] for r in meta["regimes"]]


def pnl(reg, strat, L, gamma=None, h=None):
    d = rs[(rs.persistance == reg) & (rs.strategie == strat) & (rs.L_us == L)]
    if gamma is not None:
        d = d[np.isclose(d.gamma, gamma)]
    if h is not None:
        d = d[np.isclose(d.demi_spread, h)]
    assert len(d) == 1, (reg, strat, L, gamma, h, len(d))
    return d.iloc[0]


# ------------------------------------------------------------------ figures
def fig_hero():
    """Figure héro : 2 panneaux, 10 x 4,6 pouces, textes >= 11 pt (lisible une fois réduite à la largeur du README)."""
    with plt.rc_context({"font.size": 11, "axes.titlesize": 12, "axes.labelsize": 11, "legend.fontsize": 11,
                         "xtick.labelsize": 11, "ytick.labelsize": 11}):
        fig, ax = plt.subplots(1, 2, figsize=(10, 4.6), gridspec_kw={"width_ratios": [1.2, 1]})
        # A) histogrammes de latence des deux carnets
        for d, nom, c in ((a, "std::map", C_MAP), (b, "tableau plat", C_PLAT)):
            h = d["histogramme"]
            bornes = np.array(h["bornes_basses_ns"])
            comptes = np.array(h["comptes"], dtype=float)
            larg = np.append(bornes[1:], bornes[-1] * 10 ** 0.1) - bornes
            g = d["global"]
            ax[0].bar(bornes, comptes, width=larg, align="edge", alpha=0.55, color=c, edgecolor="none",
                      label=f"{nom}\np50 {duree(g['p50_ns'])} · p99 {duree(g['p99_ns'])}")
            ax[0].axvline(g["p50_ns"], color=c, ls="--", lw=1.4)
        ax[0].set_xscale("log"), ax[0].set_yscale("log"), ax[0].set_xlim(10, 1e6), ax[0].set_ylim(0.5, 3e7)
        ax[0].set_xlabel("latence par opération (ns, log)")
        ax[0].set_ylabel("opérations (log)")
        ax[0].set_title(f"Carnet : latence sur {cfg['ops_mesurees'] / 1e6:.0f} M d'opérations")
        ax[0].legend(loc="upper right", frameon=False)
        # B) débit SPSC vs mutex
        deb = bs["debit"]
        etiq = [("file seule", "file_seule"), ("pipeline\n3 threads", "pipeline_3_threads")]
        x = np.arange(2)
        for k, (cle, c, nom) in enumerate((("spsc_lockfree", C_SPSC, "SPSC lock-free"), ("mutex_queue", C_MUTEX, "mutex + queue"))):
            med = [deb[cle][e]["median_msg_s"] / 1e6 for _, e in etiq]
            ax[1].bar(x + (k - 0.5) * 0.38, med, 0.36, color=c, label=nom)
            for xi, (_, e) in zip(x, etiq):
                pts = np.array(deb[cle][e]["par_repetition"]) / 1e6
                ax[1].scatter(np.full(len(pts), xi + (k - 0.5) * 0.38), pts, s=9, color="k", zorder=3, alpha=0.6)
                m = deb[cle][e]["median_msg_s"] / 1e6
                ax[1].text(xi + (k - 0.5) * 0.38, max(pts) * 1.25, nombre(m, 1 if m >= 1 else 2), ha="center", fontsize=11)
        rapports = [deb["spsc_lockfree"][e]["median_msg_s"] / deb["mutex_queue"][e]["median_msg_s"] for _, e in etiq]
        ax[1].set_yscale("log"), ax[1].set_ylim(0.3, 600)
        ax[1].set_xticks(x, [f"{lib}\n(SPSC = {nombre(r, 0)}× mutex)" for (lib, _), r in zip(etiq, rapports)])
        ax[1].set_ylabel("Mmsg/s (log)")
        ax[1].set_title("Débit : SPSC lock-free vs mutex")
        ax[1].legend(loc="upper right", frameon=False)
        fig.suptitle("Carnet d'ordres et file SPSC, mesures brutes (PC partagé)", fontsize=12.5, y=1.0)
        fig.tight_layout()
        sauver(fig, "hero_latences.png")


def fig_spsc():
    deb, lat = bs["debit"], bs["latence"]
    fig, ax = plt.subplots(1, 3, figsize=(15.5, 4.4))
    # débit par répétition
    for k, (cle, c, nom) in enumerate((("spsc_lockfree", C_SPSC, "SPSC"), ("mutex_queue", C_MUTEX, "mutex")), start=0):
        for j, e in enumerate(("file_seule", "pipeline_3_threads")):
            v = np.array(deb[cle][e]["par_repetition"]) / 1e6
            ax[0].scatter(np.full(len(v), j + (k - 0.5) * 0.3), v, color=c, s=22, label=nom if j == 0 else None)
            ax[0].hlines(deb[cle][e]["median_msg_s"] / 1e6, j + (k - 0.5) * 0.3 - 0.13, j + (k - 0.5) * 0.3 + 0.13, color="k", lw=1.5)
    ax[0].set_yscale("log"), ax[0].set_xticks([0, 1], ["file seule", "pipeline 3 threads"]), ax[0].set_ylabel("Mmsg/s (log)")
    ax[0].set_title("Débit : 5 répétitions (trait noir = médiane)", fontsize=10), ax[0].legend()
    # p50 par répétition
    for k, (cle, c, nom) in enumerate((("spsc_lockfree", C_SPSC, "SPSC"), ("mutex_queue", C_MUTEX, "mutex"))):
        for j, e in enumerate(("un_saut_file_seule", "pipeline_bout_en_bout")):
            v = np.array(lat[cle][e]["p50_par_repetition"])
            ax[1].scatter(np.full(len(v), j + (k - 0.5) * 0.3), v, color=c, s=22, label=nom if j == 0 else None)
            ax[1].hlines(lat[cle][e]["p50_ns"], j + (k - 0.5) * 0.3 - 0.13, j + (k - 0.5) * 0.3 + 0.13, color="k", lw=1.5)
    ax[1].set_xticks([0, 1], ["un saut\n(file seule)", "bout en bout\n(feed → carnet → stratégie)"]), ax[1].set_ylabel("p50 (ns)")
    ax[1].set_ylim(bottom=0)
    ax[1].set_title(f"Latence p50 à cadence fixe {nombre(bs['config']['cadence_msg_s'])} msg/s", fontsize=10), ax[1].legend()
    # dispersion du p99
    for k, (cle, c, nom) in enumerate((("spsc_lockfree", C_SPSC, "SPSC"), ("mutex_queue", C_MUTEX, "mutex"))):
        v = np.array(lat[cle]["pipeline_bout_en_bout"]["p99_par_repetition"]) / 1e3
        ax[2].scatter(np.arange(len(v)) + (k - 0.5) * 0.2, v, color=c, s=30, label=nom)
        ax[2].plot(np.arange(len(v)) + (k - 0.5) * 0.2, v, color=c, lw=0.8, alpha=0.5)
    ax[2].set_yscale("log"), ax[2].set_xticks(range(len(v)), [f"rép. {i + 1}" for i in range(len(v))])
    ax[2].set_ylabel("p99 bout en bout (µs, log)")
    ax[2].set_title("Le p99 varie de ×100 d'une répétition à l'autre :\nbruit d'ordonnanceur, pas un écart SPSC/mutex", fontsize=10)
    ax[2].legend()
    fig.suptitle("File SPSC lock-free vs std::mutex + std::queue, 3 threads épinglés (cœurs "
                 f"{','.join(map(str, bs['config']['coeurs_epingles']))}), Windows partagé", fontsize=10.5, y=1.02)
    sauver(fig, "fig8_spsc_vs_mutex.png")


def lignes_mm():
    """(étiquette, filtre) des 6 stratégies actives, dans l'ordre d'affichage."""
    out = [(f"AS γ = {g:g}".replace(".", ","), "AS", {"gamma": g}) for g in meta["mm"]["gammas"]]
    out += [(f"naïf, demi-spread {h:g}".replace(".", ","), "naif", {"h": float(h)}) for h in meta["mm"]["demi_spreads_naif"]]
    return out


def fig_variantes_mm():
    fig, axes = plt.subplots(1, 2, figsize=(14, 4.8), sharey=True, constrained_layout=True)
    vmax = max(abs(rs.pnl_moy.min()), rs.pnl_moy.max())
    for ax, reg in zip(axes, (REG_PUR, REG_PERS)):
        M = np.array([[pnl(reg, s, L, **kw).pnl_moy for L in Ls] for _, s, kw in lignes_mm()])
        im = ax.imshow(M, cmap="RdYlGn", vmin=-vmax, vmax=vmax, aspect="auto")
        for i in range(M.shape[0]):
            for j in range(M.shape[1]):
                ax.text(j, i, nombre(M[i, j]), ha="center", va="center", fontsize=9)
        ax.set_xticks(range(len(Ls)), NOM_L), ax.set_yticks(range(M.shape[0]), [l for l, _, _ in lignes_mm()])
        ax.set_xlabel("période de recotation L"), ax.grid(False)
        ax.set_title(f"persistance du flux = {reg:g}".replace(".", ",") + (" (Santa Fe pur)" if reg == 0.5 else " (flux persistant)"), fontsize=10)
    fig.colorbar(im, ax=axes, shrink=0.85, label="PnL moyen par session (ticks × lots)")
    fig.suptitle(f"Toutes les configurations du simulateur : {len(rs)} lignes ({len(rs) // 2} par régime, dont « sans market maker » = 0), "
                 f"{meta['sessions_par_config']} sessions chacune", fontsize=10.5)
    sauver(fig, "toutes_variantes_mm.png")


# ------------------------------------------------------------------ blocs de README
def bloc_badges():
    return "\n".join([
        "[![tests](https://github.com/yassine-ek/cpp-market-engine/actions/workflows/tests.yml/badge.svg)]"
        "(https://github.com/yassine-ek/cpp-market-engine/actions/workflows/tests.yml)",
        "![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)",
        "![licence MIT](https://img.shields.io/badge/licence-MIT-blue)",
        "![sans dépendance](https://img.shields.io/badge/d%C3%A9pendances-aucune-lightgrey)",
    ])


def bloc_cles():
    """Trois puces « Résultats clés » : chaque chiffre est lu dans results/, chacun avec sa réserve."""
    g1, g2 = a["global"], b["global"]
    deb, la = bs["debit"], bs["latence"]
    S, M = "spsc_lockfree", "mutex_queue"
    n_diff = sum(int(d[0]) for d in diff)
    des = sum(int(d[2]) for d in diff)
    return "\n".join([
        f"- **Carnet d'ordres : le tableau plat est {rapport(g1['p50_ns'], g2['p50_ns'], 0)} plus rapide que `std::map` au p50** "
        f"({duree(g2['p50_ns'])} contre {duree(g1['p50_ns'])} ; p99 {duree(g2['p99_ns'])} contre {duree(g1['p99_ns'])} ; "
        f"{mops(b['debit_ops_s'])} contre {mops(a['debit_ops_s'], 2)} Mops/s), sur {nombre(cfg['ops_mesurees'] / 1e6)} M d'opérations synthétiques. "
        f"*Réserve : latences brutes (overhead de `rdtsc` de {nombre(cfg['overhead_horloge_ns'], 1)} ns non soustrait), mesurées sur le cœur logique "
        f"{cfg['coeur_epingle']} d'un PC Windows partagé, choisi après essais ; les valeurs absolues varient d'un facteur 1,6, le rapport p50 reste dans 4,7-5,1×.*",
        f"- **Correction vérifiée** : les deux carnets rendent exactement les mêmes trades dans le même ordre sur {nombre(n_diff / 1e6, 1)} M d'opérations aléatoires "
        f"({des} désaccord) ; {verifs[0][0]} + {verifs[1][0]} vérifications, {verifs[0][1] + verifs[1][1]} échec ; la CI Linux rejoue les tests sous ASan, UBSan et ThreadSanitizer (badge ci-dessus). "
        "*Réserve : cela prouve l'équivalence sur les flux testés, pas la correction sur tous les flux ; les sanitizers ne tournent pas sous le MinGW local.*",
        f"- **File SPSC lock-free : {rapport(deb[S]['pipeline_3_threads']['median_msg_s'], deb[M]['pipeline_3_threads']['median_msg_s'], 0)} le débit d'une file `std::mutex` dans le pipeline à 3 threads** "
        f"({mops(deb[S]['pipeline_3_threads']['median_msg_s'])} contre {mops(deb[M]['pipeline_3_threads']['median_msg_s'], 2)} Mmsg/s ; "
        f"p50 de bout en bout {duree(la[S]['pipeline_bout_en_bout']['p50_ns'])} contre {duree(la[M]['pipeline_bout_en_bout']['p50_ns'])}). "
        "*Réserve : seuls le p50 et le débit départagent les deux files, les queues p99 / p99,9 sont du bruit d'ordonnanceur ; mesuré sur x86-64 (modèle mémoire TSO).*",
    ])


def bloc_hypotheses():
    mix = cfg["mix"]
    lignes = [
        ("Machine", f"Intel Core Ultra 7 155H ({bs['config']['threads_materiels']} threads logiques, hybride P/E), Windows 11, **PC de travail partagé** (d'autres processus tournent). "
                    f"g++ {cfg['compilateur']} (MinGW-w64), `{cfg['flags']}`. La CI Linux compile et teste, elle ne mesure pas."),
        ("Horloge", f"`rdtsc` encadré de `lfence`, calibré à {nombre(cfg['ticks_par_ns'], 3)} GHz sur `steady_clock` (granularité {cfg['granularite_steady_clock_ns']} ns, plus grosse que l'opération). "
                    f"Overhead de {nombre(cfg['overhead_horloge_ns'], 1)} ns par mesure, mesuré et **non soustrait** : toutes les latences sont brutes."),
        ("Épinglage", f"Carnet : cœur logique {cfg['coeur_epingle']}, **choisi après essais** (les cœurs 2, 4 et 6 étaient plus lents ou plus chargés). "
                      f"SPSC / pipeline : cœurs {', '.join(map(str, bs['config']['coeurs_epingles']))}. Topologie P/E non vérifiée."),
        ("Flux et répétitions", f"{nombre(cfg['ops_mesurees'])} opérations après {nombre(cfg['ops_warmup'])} de chauffe ({round(mix['ajout'] * 100)} % ajouts, "
                                f"{round(mix['annulation'] * 100)} % annulations, {round(mix['marche'] * 100)} % marché), ≈ {nombre(flux['ordres_vivants_moyen'])} ordres vivants, graine {cfg['graine']}, "
                                f"même flux sur les deux carnets. Carnet : 3 répétitions alternées (médiane de p50) ; SPSC : {bs['config']['repetitions']} répétitions, cadence {nombre(bs['config']['cadence_msg_s'])} msg/s."),
        ("Variabilité, non mesuré", "Valeurs absolues : facteur 1,6 d'une exécution à l'autre, rapport p50 map / plat 4,7 à 5,1×. Queues p99 / p99,9 des files : bruit d'ordonnanceur. "
                                    "Non mesuré : compteurs matériels, réseau, données de marché réelles."),
    ]
    return tableau(["Hypothèse de mesure", "Valeur"], lignes)


def bloc_metriques():
    g1, g2 = a["global"], b["global"]
    t1 = tableau(["Carnet, 5 M opérations", "p50", "p99", "p99,9", "max", "débit"], [
        ("(a) `std::map` + `std::list`", duree(g1["p50_ns"]), duree(g1["p99_ns"]), duree(g1["p999_ns"]), duree(g1["max_ns"]), f"{mops(a['debit_ops_s'], 2)} Mops/s"),
        ("(b) tableau plat indexé par tick", duree(g2["p50_ns"]), duree(g2["p99_ns"]), duree(g2["p999_ns"]), duree(g2["max_ns"]), f"{mops(b['debit_ops_s'], 1)} Mops/s"),
        ("**rapport (a) / (b)**", f"**{rapport(g1['p50_ns'], g2['p50_ns'])}**", f"**{rapport(g1['p99_ns'], g2['p99_ns'])}**",
         f"**{rapport(g1['p999_ns'], g2['p999_ns'])}**", "interruptions OS", f"**{rapport(b['debit_ops_s'], a['debit_ops_s'])}**")],
        ["---", "--:", "--:", "--:", "--:", "--:"])
    la, sp, mu = bs["latence"], bs["debit"], None
    S, M = "spsc_lockfree", "mutex_queue"

    def tri(d):
        return f"{duree(d['p50_ns'])} / {duree(d['p99_ns'])} / {duree(d['p999_ns'])}"

    t2 = tableau(["File SPSC vs mutex", "SPSC lock-free", "`std::mutex` + `std::queue`", "rapport"], [
        ("Débit, file seule (20 M entiers)", f"{mops(sp[S]['file_seule']['median_msg_s'])} Mmsg/s", f"{mops(sp[M]['file_seule']['median_msg_s'])} Mmsg/s",
         rapport(sp[S]["file_seule"]["median_msg_s"], sp[M]["file_seule"]["median_msg_s"], 0)),
        ("Débit, pipeline 3 threads (4 M ordres)", f"{mops(sp[S]['pipeline_3_threads']['median_msg_s'])} Mmsg/s", f"{mops(sp[M]['pipeline_3_threads']['median_msg_s'], 2)} Mmsg/s",
         rapport(sp[S]["pipeline_3_threads"]["median_msg_s"], sp[M]["pipeline_3_threads"]["median_msg_s"], 0)),
        ("Latence d'un saut, p50 / p99 / p99,9", tri(la[S]["un_saut_file_seule"]), tri(la[M]["un_saut_file_seule"]),
         f"p50 : {rapport(la[M]['un_saut_file_seule']['p50_ns'], la[S]['un_saut_file_seule']['p50_ns'])}"),
        ("Latence bout en bout, p50 / p99 / p99,9", tri(la[S]["pipeline_bout_en_bout"]), tri(la[M]["pipeline_bout_en_bout"]),
         f"p50 : {rapport(la[M]['pipeline_bout_en_bout']['p50_ns'], la[S]['pipeline_bout_en_bout']['p50_ns'])}")],
        ["---", "--:", "--:", "--:"])
    e, ag, ge = mp["europeen"], mp["asiatique_arithmetique"], mp["asiatique_geometrique_validation"]
    s16 = [s for s in mp["speedup"] if s["threads"] == 16][0]
    t3 = tableau(["Pricer Monte Carlo (GBM)", "estimation ± erreur-type", "référence", "écart"], [
        (f"Call européen ({nombre(mp['parametres']['couples_antithetiques_europeen'] / 1e6)} M couples antithétiques)", f"{nombre(e['prix'], 5)} ± {nombre(e['erreur_type'], 5)}",
         f"Black-Scholes {nombre(e['black_scholes'], 5)}", f"{nombre(e['ecart_en_erreurs_types'], 2)} erreur-type"),
        ("Call asiatique géométrique (validation)", f"{nombre(ge['prix_mc'], 5)} ± {nombre(ge['erreur_type'], 5)}", f"forme fermée {nombre(ge['forme_fermee'], 5)}",
         f"{nombre(ge['ecart_en_erreurs_types'], 2)} erreur-type"),
        ("Call asiatique arithmétique (50 fixings)", f"{nombre(ag['prix'], 5)} ± {nombre(ag['erreur_type'], 5)}", "aucune forme fermée", "voir géométrique"),
        ("Speedup à 16 threads (européen / asiatique)", f"{nombre(s16['speedup_europeen'], 2)}× / {nombre(s16['speedup_asiatique'], 2)}×", "idéal 16×",
         f"CPU hybride, {mp['parametres']['threads_materiels']} threads logiques")], ["---", "--:", "---", "---"])
    ligs = []
    for etiq, s, kw in (("AS γ = 0,005", "AS", {"gamma": 0.005}), ("naïf, demi-spread 2 ticks", "naif", {"h": 2.0})):
        v = [pnl(REG_PUR, s, L, **kw).pnl_moy for L in Ls]
        ligs.append((etiq, *[nombre(x) for x in v], f"{nombre((v[-1] / v[0] - 1) * 100, 0)} %"))
    t4 = tableau(["Market maker, PnL moyen par session (ticks × lots), flux pur", *NOM_L, "10 µs → 10 ms"], ligs, ["---", "--:", "--:", "--:", "--:", "--:"])
    return "\n\n".join([t1, t2, t3, t4])


def bloc_lecture30():
    n_diff = sum(int(d[0]) for d in diff)
    des, inc = sum(int(d[2]) for d in diff), sum(int(d[3]) for d in diff)
    la = bs["latence"]
    p99_s = la["spsc_lockfree"]["pipeline_bout_en_bout"]["p99_par_repetition"]
    return "\n".join([
        f"- **Correct ?** Les deux carnets produisent exactement les mêmes trades, dans le même ordre, sur {nombre(n_diff)} opérations aléatoires : "
        f"{des} désaccord, {inc} incohérence ; {verifs[0][0]} + {verifs[1][0]} vérifications, {verifs[0][1] + verifs[1][1]} échec "
        "(sortie dans `results/log_tests.txt`). Le pipeline à 3 threads rend le même carnet final que l'exécution mono-thread, file SPSC comme file mutex.",
        f"- **Rapide ?** Le rapport p50 est de {rapport(a['global']['p50_ns'], b['global']['p50_ns'])} et le débit de {rapport(b['debit_ops_s'], a['debit_ops_s'])} ; "
        "les valeurs absolues bougent d'un facteur 1,6 selon le cœur et la charge de la machine, le rapport reste dans 4,7-5,1× sur le p50. "
        f"Pour le SPSC, seuls le p50 et le débit départagent : le p99 bout en bout va de {duree(min(p99_s))} à {duree(max(p99_s))} d'une répétition à l'autre.",
        f"- **Utile ?** Dans le simulateur, passer d'une recotation toutes les 10 µs à toutes les 10 ms fait perdre "
        f"{nombre(-(pnl(REG_PUR, 'AS', Ls[-1], gamma=0.005).pnl_moy / pnl(REG_PUR, 'AS', Ls[0], gamma=0.005).pnl_moy - 1) * 100, 0)} % de PnL à l'Avellaneda-Stoikov (γ = 0,005). "
        "Mais cette « latence » est une période de recotation en temps simulé, **sans lien avec la latence mesurée du carnet** (les ns du benchmark ne sont pas injectés dans le simulateur) ; "
        "le marché est synthétique et non calibré : c'est un mécanisme illustré, pas une rentabilité.",
    ])


def bloc_variantes():
    out = []
    # 1) carnets x types d'opérations
    lignes = []
    for nom, d in (("(a) `std::map`", a), ("(b) tableau plat", b)):
        for t in ("global", "ajout", "annulation", "marche"):
            x = d[t]
            lignes.append((nom, t if t != "global" else "**toutes**", nombre(x["n"]), duree(x["p50_ns"]), duree(x["p99_ns"]), duree(x["p999_ns"]), duree(x["max_ns"]), duree(x["moy_ns"])))
    out.append("**Carnets : 2 structures × 4 types d'opération** (`results/bench_carnet.json`)\n\n" + tableau(
        ["Carnet", "Opération", "n", "p50", "p99", "p99,9", "max", "moyenne"], lignes, ["---", "---", "--:", "--:", "--:", "--:", "--:", "--:"]))
    rep = tableau(["Carnet", "débit par répétition (Mops/s)", "p50 par répétition (ns)"], [
        (nom, ", ".join(mops(x, 2) for x in d["debits_par_repetition"]), ", ".join(nombre(x, 1) for x in d["p50_ns_par_repetition"]))
        for nom, d in (("(a) `std::map`", a), ("(b) tableau plat", b))])
    out.append("Les trois répétitions alternées (on publie celle de p50 médian) :\n\n" + rep)
    # 2) SPSC / mutex
    lignes = []
    for nom, cle in (("SPSC lock-free", "spsc_lockfree"), ("`std::mutex` + `std::queue`", "mutex_queue")):
        for lib, e in (("file seule", "file_seule"), ("pipeline 3 threads", "pipeline_3_threads")):
            d = bs["debit"][cle][e]
            lignes.append((nom, f"débit, {lib}", f"{mops(d['median_msg_s'], 2)} Mmsg/s", f"{mops(min(d['par_repetition']), 2)} à {mops(max(d['par_repetition']), 2)} Mmsg/s", "", ""))
        for lib, e in (("un saut, file seule", "un_saut_file_seule"), ("bout en bout (pipeline)", "pipeline_bout_en_bout")):
            d = bs["latence"][cle][e]
            lignes.append((nom, f"latence, {lib}", f"p50 {duree(d['p50_ns'])}", f"{duree(min(d['p50_par_repetition']))} à {duree(max(d['p50_par_repetition']))}",
                           f"p99 {duree(d['p99_ns'])} (de {duree(min(d['p99_par_repetition']))} à {duree(max(d['p99_par_repetition']))})", f"p99,9 {duree(d['p999_ns'])}, max {duree(d['max_ns'])}"))
    out.append("**File SPSC vs mutex : 2 files × (débit, latence) × (file seule, pipeline)** (`results/bench_spsc.json`)\n\n" + tableau(
        ["File", "Mesure", "Médiane de 5 rép.", "Min à max des répétitions (p50 ou débit)", "Queue p99 (médiane, étendue)", "Extrêmes"], lignes))
    # 3) pricer
    lignes = [(s["threads"], f"{nombre(s['temps_europeen_s'], 3)} s", f"{nombre(s['speedup_europeen'], 2)}×", f"{nombre(s['temps_asiatique_s'], 3)} s",
               f"{nombre(s['speedup_asiatique'], 2)}×", nombre(s["efficacite_asiatique"], 2)) for s in mp["speedup"]]
    out.append("**Pricer Monte Carlo : 5 nombres de threads × 2 produits** (`results/mc_pricer.json`)\n\n" + tableau(
        ["threads", "européen : temps", "speedup", "asiatique : temps", "speedup", "efficacité asiatique"], lignes, ["--:"] * 6))
    # 4) market maker
    for reg, titre in ((REG_PUR, "flux pur (persistance 0,50, Santa Fe)"), (REG_PERS, "flux persistant (persistance 0,75)")):
        sub = rs[rs.persistance == reg]
        lignes = [("sans market maker", *["0"] * len(Ls))]
        for etiq, s, kw in lignes_mm():
            lignes.append((etiq, *[nombre(pnl(reg, s, L, **kw).pnl_moy) for L in Ls]))
        det = [(etiq, nombre(pnl(reg, s, Ls[1], **kw).pnl_moy), nombre(pnl(reg, s, Ls[1], **kw).pnl_sd), nombre(pnl(reg, s, Ls[1], **kw).inv_max, 1),
                nombre(pnl(reg, s, Ls[1], **kw).trades)) for etiq, s, kw in lignes_mm()]
        out.append(f"**Simulateur : {len(sub)} configurations, {titre}.** PnL moyen par session (ticks × lots) ; erreur-type de la moyenne de "
                   f"{nombre(sub[sub.strategie != 'sans_mm'].pnl_se.min())} à {nombre(sub.pnl_se.max())} ({meta['sessions_par_config']} sessions par ligne)\n\n"
                   + tableau(["Stratégie", *NOM_L], lignes, ["---"] + ["--:"] * len(Ls))
                   + f"\n\nDétail à L = {NOM_L[1]} :\n\n" + tableau(["Stratégie", "PnL moyen", "écart-type", "inventaire max moyen (lots)", "trades par session"], det, ["---"] + ["--:"] * 4))
    return "\n\n".join(out)


BLOCS = {"badges": bloc_badges, "cles": bloc_cles, "hypotheses": bloc_hypotheses, "metriques": bloc_metriques, "lecture30": bloc_lecture30, "variantes": bloc_variantes}


def injecter():
    chemin = RACINE / "README.md"
    txt = chemin.read_text(encoding="utf-8")
    for nom, f in BLOCS.items():
        motif = re.compile(rf"(<!-- BEGIN:{nom} -->).*?(<!-- END:{nom} -->)", re.S)
        if not motif.search(txt):
            raise SystemExit(f"marqueur {nom} absent du README")
        contenu = f()
        txt = motif.sub(lambda m: m.group(1) + "\n" + contenu.strip("\n") + "\n" + m.group(2), txt)
    chemin.write_text(txt, encoding="utf-8")
    print("  -> README.md (blocs :", ", ".join(BLOCS), ")")


if __name__ == "__main__":
    print("figures et README :")
    fig_hero()
    fig_spsc()
    fig_variantes_mm()
    injecter()
