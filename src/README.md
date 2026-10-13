# src/ : implémentations et exécutables

| Fichier | Rôle | Produit |
|---|---|---|
| `carnet.cpp` | Les deux implémentations du carnet (`CarnetMap` « manuel de cours », `CarnetPlat` tableau plat). Compilé une fois en `build/carnet.o`, lié aux tests, aux benchmarks et au simulateur. | `build/carnet.o` |
| `simulateur.cpp` | Simulation événementielle multi-agents sur le carnet plat : traders zéro-intelligence (Poisson), market maker Avellaneda-Stoikov, market maker naïf, sans market maker ; calibration de σ² et k ; 500 sessions Monte Carlo par configuration sur N threads (la graine d'une session ne dépend que de son indice). | `results/sessions.csv`, `trajectoire.csv`, `serie_mid.csv`, `simulateur_meta.json` |
| `mc_pricer.cpp` | Pricer Monte Carlo multithread (call européen, call asiatique) : un `mt19937_64` par thread, variables antithétiques, accumulation locale ; validation contre Black-Scholes et l'asiatique géométrique en forme fermée ; mesure du speedup de 1 à 16 threads. | `results/mc_pricer.json` |

Lancer : `bash build.sh` puis `./build/simulateur` et `./build/mc_pricer` (voir le README de la racine).
