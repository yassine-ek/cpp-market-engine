# results/ : sorties des exécutables

Fichiers produits par `build/bench_*`, `build/simulateur`, `build/mc_pricer` et par `analyse/`. Les chiffres du README de la racine sont lus ici. Les valeurs changent d'une machine à l'autre (voir « Hypothèses de mesure » dans le README de la racine).

| Fichier | Produit par | Contenu |
|---|---|---|
| `bench_carnet.json` | `bench_carnet` | Configuration, flux, percentiles et histogramme par type d'opération pour les deux carnets, débits et p50 par répétition, rapports (a)/(b). |
| `bench_spsc.json` | `bench_spsc` | Débit (file seule, pipeline) et latences (un saut, bout en bout) du SPSC et du mutex, médiane et valeurs par répétition. |
| `mc_pricer.json` | `mc_pricer` | Prix européen / asiatique avec erreur-type, validation contre Black-Scholes et l'asiatique géométrique, temps et speedup par nombre de threads. |
| `simulateur_meta.json` | `simulateur` | Grille (L, γ, demi-spreads), calibration de σ² et k pour chaque régime de flux, temps de calcul. |
| `sessions.csv` | `simulateur` | Une ligne par session (25 000) : régime, stratégie, L, γ, demi-spread, PnL, inventaire max, trades, spread moyen. |
| `resume_pnl.csv` | `analyse/analyse.py` | Agrégat de `sessions.csv` par configuration (50 lignes) : PnL moyen, écart-type, erreur-type, inventaire, trades. |
| `trajectoire.csv` | `simulateur` | Une session détaillée (mid, meilleurs bid / ask, cotes du market maker, inventaire, PnL mark-to-market). |
| `serie_mid.csv` | `simulateur` | 100 s de mid échantillonné toutes les 1 ms, sans market maker (faits stylisés). |
| `log_tests.txt` | `test_carnet`, `test_spsc` | Sortie complète des deux suites de tests (134 et 162 vérifications). |
| `log_bench.txt`, `log_bench_spsc.txt`, `log_mc_pricer.txt`, `log_simulateur.txt` | exécutables | Sortie console des runs publiés. |

Taille : tous les CSV font moins de 2 Mo. Tout CSV de plus de 5 Mo doit être écrit dans `results/gros/` (ignoré par git).
