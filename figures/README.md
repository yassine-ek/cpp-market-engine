# figures/ : une ligne par image

PNG en 130 dpi, produits à partir de `results/`. Chaque image est commentée (« Comment lire ce graphique ») dans le README de la racine ou dans `docs/RESULTATS_DETAILLES.md`.

| Image | Ce qu'elle montre | Script |
|---|---|---|
| `hero_latences.png` | Figure héro (2 panneaux, textes ≥ 11 pt) : histogramme des latences des deux carnets, débit SPSC contre mutex. La lecture détaillée est dans `docs/RESULTATS_DETAILLES.md`. | `analyse/vitrine_05.py` |
| `toutes_variantes_mm.png` | Carte de chaleur du PnL moyen des 24 configurations de market maker (+ « sans ») pour les deux régimes de flux. | `analyse/vitrine_05.py` |
| `fig1_histogramme_latences.png` | Histogrammes log-log de la latence par opération, `std::map` contre tableau plat, avec p50 et p99. | `analyse/analyse.py` |
| `fig2_comparaison_carnets.png` | Percentiles globaux, p50 par type d'opération et débit des deux carnets. | `analyse/analyse.py` |
| `fig3_pnl_vs_latence.png` | Boîtes de PnL par période de recotation : AS et naïf, deux régimes de flux. | `analyse/analyse.py` |
| `fig4_pnl_as_vs_naif.png` | Distribution du PnL à L = 100 µs et plan rendement / risque de toutes les configurations. | `analyse/analyse.py` |
| `fig5_trajectoire.png` | Une session de 2 s : mid, cotes du market maker, zoom de 100 ms, inventaire, PnL. | `analyse/analyse.py` |
| `fig6_faits_stylises.png` | Queues épaisses et autocorrélations des rendements du mid simulé. | `analyse/analyse.py` |
| `fig7_speedup_pricer.png` | Speedup du pricer Monte Carlo de 1 à 16 threads (européen, asiatique) contre l'idéal. | `analyse/analyse.py` |
| `fig8_spsc_vs_mutex.png` | SPSC contre mutex : débit, p50 par répétition, dispersion du p99 bout en bout. | `analyse/vitrine_05.py` |
