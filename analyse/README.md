# analyse/ : figures et tableaux (Python)

Dépendances : `pip install -r requirements.txt` (numpy, pandas, matplotlib). À lancer depuis la racine du projet. Les scripts ne font que **lire** `results/` ; aucun chiffre du README n'est tapé à la main.

| Fichier | Rôle | Produit |
|---|---|---|
| `analyse.py` | Figures historiques : latence des carnets, simulateur (PnL, trajectoire, faits stylisés), speedup du pricer ; agrège `sessions.csv` par configuration. | `figures/fig1` à `fig7`, `results/resume_pnl.csv` |
| `vitrine_05.py` | Figures de vitrine (héro, SPSC contre mutex, carte de chaleur de toutes les configurations) et **réécriture des blocs de tableaux du `README.md`** (badges, hypothèses de mesure, métriques, lecture en 30 secondes, toutes les variantes) entre les marqueurs `<!-- BEGIN:nom -->` et `<!-- END:nom -->`. Lit aussi `results/log_tests.txt` pour le nombre de vérifications. | `figures/hero_latences.png`, `fig8_spsc_vs_mutex.png`, `toutes_variantes_mm.png`, blocs du README |
