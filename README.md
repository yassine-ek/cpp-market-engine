# Moteur de marché C++20 : carnet d'ordres, file lock-free, market maker, pricer Monte Carlo

Un petit moteur de marché en **C++20 sans dépendance** : deux carnets d'ordres à priorité prix-temps (`std::map` contre tableau plat) comparés au p50 / p99 / p99,9 et testés l'un contre l'autre, une file SPSC lock-free contre `std::mutex` dans un pipeline feed → carnet → stratégie, un simulateur multi-agents avec market maker Avellaneda-Stoikov, et un pricer Monte Carlo multithread.

> **At a glance.** A dependency-free C++20 market engine: two price-time order books (`std::map` vs flat tick array) benchmarked at p50/p99/p99.9 and tested differentially,
> a lock-free SPSC queue vs `std::mutex` in a 3-thread pipeline, a multi-agent simulator with an Avellaneda-Stoikov market maker, and a multithreaded Monte Carlo pricer.
> Latencies are raw (rdtsc overhead of 15 ns not subtracted), measured on one pinned core of a shared Windows PC: ratios are more stable than absolute values.

<!-- BEGIN:badges -->
[![tests](https://github.com/yassine-ek/cpp-market-engine/actions/workflows/tests.yml/badge.svg)](https://github.com/yassine-ek/cpp-market-engine/actions/workflows/tests.yml)
![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)
![licence MIT](https://img.shields.io/badge/licence-MIT-blue)
![sans dépendance](https://img.shields.io/badge/d%C3%A9pendances-aucune-lightgrey)
<!-- END:badges -->

## Résultats clés

<!-- BEGIN:cles -->
- **Carnet d'ordres : le tableau plat est 5× plus rapide que `std::map` au p50** (42 ns contre 214 ns ; p99 290 ns contre 1,7 µs ; 33,4 contre 3,26 Mops/s), sur 5 M d'opérations synthétiques. *Réserve : latences brutes (overhead de `rdtsc` de 15,4 ns non soustrait), mesurées sur le cœur logique 14 d'un PC Windows partagé, choisi après essais ; les valeurs absolues varient d'un facteur 1,6, le rapport p50 reste dans 4,7-5,1×.*
- **Correction vérifiée** : les deux carnets rendent exactement les mêmes trades dans le même ordre sur 1,2 M d'opérations aléatoires (0 désaccord) ; 134 + 162 vérifications, 0 échec ; la CI Linux rejoue les tests sous ASan, UBSan et ThreadSanitizer (badge ci-dessus). *Réserve : cela prouve l'équivalence sur les flux testés, pas la correction sur tous les flux ; les sanitizers ne tournent pas sous le MinGW local.*
- **File SPSC lock-free : 9× le débit d'une file `std::mutex` dans le pipeline à 3 threads** (6,9 contre 0,76 Mmsg/s ; p50 de bout en bout 406 ns contre 1,8 µs). *Réserve : seuls le p50 et le débit départagent les deux files, les queues p99 / p99,9 sont du bruit d'ordonnanceur ; mesuré sur x86-64 (modèle mémoire TSO).*
<!-- END:cles -->

![Latence des deux carnets d'ordres et débit de la file SPSC contre mutex](figures/hero_latences.png)

*Les barres du panneau de droite sont les médianes de 5 répétitions (points noirs) ; les traits pointillés du panneau de gauche marquent le p50. [Figure et lecture détaillées](docs/RESULTATS_DETAILLES.md).*

Pour aller plus loin : [conception et architecture](docs/CONCEPTION.md) · [résultats détaillés](docs/RESULTATS_DETAILLES.md) · [positionnement](docs/POSITIONNEMENT.md). Aucun tableau de bord ni notebook : tout se lit dans `results/` et dans ce README.

## Hypothèses de mesure

À lire avant tout chiffre : les latences sont **brutes**, prises sur un **PC Windows partagé**, et le flux d'ordres est **synthétique**.

<!-- BEGIN:hypotheses -->
| Hypothèse de mesure | Valeur |
|---|---|
| Machine | Intel Core Ultra 7 155H (22 threads logiques, hybride P/E), Windows 11, **PC de travail partagé** (d'autres processus tournent). g++ 16.2.0 (MinGW-w64), `-O3 -march=native -std=c++20`. La CI Linux compile et teste, elle ne mesure pas. |
| Horloge | `rdtsc` encadré de `lfence`, calibré à 2,995 GHz sur `steady_clock` (granularité 100 ns, plus grosse que l'opération). Overhead de 15,4 ns par mesure, mesuré et **non soustrait** : toutes les latences sont brutes. |
| Épinglage | Carnet : cœur logique 14, **choisi après essais** (les cœurs 2, 4 et 6 étaient plus lents ou plus chargés). SPSC / pipeline : cœurs 4, 6, 8. Topologie P/E non vérifiée. |
| Flux et répétitions | 5 000 000 opérations après 500 000 de chauffe (70 % ajouts, 25 % annulations, 5 % marché), ≈ 50 114 ordres vivants, graine 20260101, même flux sur les deux carnets. Carnet : 3 répétitions alternées (médiane de p50) ; SPSC : 5 répétitions, cadence 300 000 msg/s. |
| Variabilité, non mesuré | Valeurs absolues : facteur 1,6 d'une exécution à l'autre, rapport p50 map / plat 4,7 à 5,1×. Queues p99 / p99,9 des files : bruit d'ordonnanceur. Non mesuré : compteurs matériels, réseau, données de marché réelles. |
<!-- END:hypotheses -->

## Tableau des métriques

Tous les nombres sont lus dans `results/*.json` et `results/resume_pnl.csv` par `analyse/vitrine_05.py`.

<!-- BEGIN:metriques -->
| Carnet, 5 M opérations | p50 | p99 | p99,9 | max | débit |
|---|--:|--:|--:|--:|--:|
| (a) `std::map` + `std::list` | 214 ns | 1,7 µs | 7,6 µs | 291 µs | 3,26 Mops/s |
| (b) tableau plat indexé par tick | 42 ns | 290 ns | 449 ns | 254 µs | 33,4 Mops/s |
| **rapport (a) / (b)** | **5,1×** | **5,9×** | **16,9×** | interruptions OS | **10,2×** |

| File SPSC vs mutex | SPSC lock-free | `std::mutex` + `std::queue` | rapport |
|---|--:|--:|--:|
| Débit, file seule (20 M entiers) | 99,7 Mmsg/s | 6,1 Mmsg/s | 16× |
| Débit, pipeline 3 threads (4 M ordres) | 6,9 Mmsg/s | 0,76 Mmsg/s | 9× |
| Latence d'un saut, p50 / p99 / p99,9 | 251 ns / 7,4 µs / 1,79 ms | 889 ns / 26,6 µs / 1,13 ms | p50 : 3,5× |
| Latence bout en bout, p50 / p99 / p99,9 | 406 ns / 64,8 µs / 923 µs | 1,8 µs / 233 µs / 1,31 ms | p50 : 4,5× |

| Pricer Monte Carlo (GBM) | estimation ± erreur-type | référence | écart |
|---|--:|---|---|
| Call européen (20 M couples antithétiques) | 8,91612 ± 0,00167 | Black-Scholes 8,91604 | 0,05 erreur-type |
| Call asiatique géométrique (validation) | 4,93494 ± 0,00395 | forme fermée 4,94185 | -1,75 erreur-type |
| Call asiatique arithmétique (50 fixings) | 5,12250 ± 0,00407 | aucune forme fermée | voir géométrique |
| Speedup à 16 threads (européen / asiatique) | 4,89× / 6,25× | idéal 16× | CPU hybride, 22 threads logiques |

| Market maker, PnL moyen par session (ticks × lots), flux pur | 10 µs | 100 µs | 1 ms | 10 ms | 10 µs → 10 ms |
|---|--:|--:|--:|--:|--:|
| AS γ = 0,005 | 1 825 | 1 801 | 1 592 | 801 | -56 % |
| naïf, demi-spread 2 ticks | 3 090 | 3 016 | 2 386 | 849 | -73 % |
<!-- END:metriques -->

## Les chiffres sont-ils fiables ? Lecture en 30 secondes

<!-- BEGIN:lecture30 -->
- **Correct ?** Les deux carnets produisent exactement les mêmes trades, dans le même ordre, sur 1 200 000 opérations aléatoires : 0 désaccord, 0 incohérence ; 134 + 162 vérifications, 0 échec (sortie dans `results/log_tests.txt`). Le pipeline à 3 threads rend le même carnet final que l'exécution mono-thread, file SPSC comme file mutex.
- **Rapide ?** Le rapport p50 est de 5,1× et le débit de 10,2× ; les valeurs absolues bougent d'un facteur 1,6 selon le cœur et la charge de la machine, le rapport reste dans 4,7-5,1× sur le p50. Pour le SPSC, seuls le p50 et le débit départagent : le p99 bout en bout va de 2,8 µs à 294 µs d'une répétition à l'autre.
- **Utile ?** Dans le simulateur, passer d'une recotation toutes les 10 µs à toutes les 10 ms fait perdre 56 % de PnL à l'Avellaneda-Stoikov (γ = 0,005). Mais cette « latence » est une période de recotation en temps simulé, **sans lien avec la latence mesurée du carnet** (les ns du benchmark ne sont pas injectés dans le simulateur) ; le marché est synthétique et non calibré : c'est un mécanisme illustré, pas une rentabilité.
<!-- END:lecture30 -->

## Toutes les variantes

Rien n'est écarté : toutes les configurations mesurées sont ci-dessous, y compris celles où l'Avellaneda-Stoikov perd de l'argent ou où le speedup plafonne.

![Carte de chaleur du PnL moyen pour les 24 configurations de market maker et les deux régimes de flux](figures/toutes_variantes_mm.png)

**Comment lire ce graphique.** Chaque case est une configuration du simulateur (une stratégie en ligne, une période de recotation L en colonne) ; le chiffre est le PnL moyen de 500 sessions et la couleur va du rouge (perte) au vert (gain). À gauche le flux pur, à droite le flux persistant : on voit le PnL baisser partout quand L augmente, et la ligne « AS γ = 0,02 » devenir négative quand le flux est persistant.

<details>
<summary>Tableaux de toutes les configurations : carnets, file SPSC, pricer, simulateur</summary>

<!-- BEGIN:variantes -->
**Carnets : 2 structures × 4 types d'opération** (`results/bench_carnet.json`)

| Carnet | Opération | n | p50 | p99 | p99,9 | max | moyenne |
|---|---|--:|--:|--:|--:|--:|--:|
| (a) `std::map` | **toutes** | 5 000 000 | 214 ns | 1,7 µs | 7,6 µs | 291 µs | 365 ns |
| (a) `std::map` | ajout | 3 498 663 | 183 ns | 493 ns | 1,2 µs | 291 µs | 199 ns |
| (a) `std::map` | annulation | 1 251 118 | 680 ns | 1,2 µs | 2,5 µs | 251 µs | 672 ns |
| (a) `std::map` | marche | 250 219 | 542 ns | 9,3 µs | 17,0 µs | 253 µs | 1,1 µs |
| (b) tableau plat | **toutes** | 5 000 000 | 42 ns | 290 ns | 449 ns | 254 µs | 62 ns |
| (b) tableau plat | ajout | 3 498 663 | 38 ns | 90 ns | 186 ns | 254 µs | 41 ns |
| (b) tableau plat | annulation | 1 251 118 | 62 ns | 347 ns | 481 ns | 211 µs | 107 ns |
| (b) tableau plat | marche | 250 219 | 105 ns | 467 ns | 839 ns | 22,2 µs | 131 ns |

Les trois répétitions alternées (on publie celle de p50 médian) :

| Carnet | débit par répétition (Mops/s) | p50 par répétition (ns) |
|---|---|---|
| (a) `std::map` | 3,23, 3,53, 3,26 | 218,3, 208,3, 213,7 |
| (b) tableau plat | 27,98, 33,79, 33,41 | 42,1, 42,1, 42,1 |

**File SPSC vs mutex : 2 files × (débit, latence) × (file seule, pipeline)** (`results/bench_spsc.json`)

| File | Mesure | Médiane de 5 rép. | Min à max des répétitions (p50 ou débit) | Queue p99 (médiane, étendue) | Extrêmes |
|---|---|---|---|---|---|
| SPSC lock-free | débit, file seule | 99,74 Mmsg/s | 79,38 à 133,47 Mmsg/s |  |  |
| SPSC lock-free | débit, pipeline 3 threads | 6,87 Mmsg/s | 5,95 à 8,13 Mmsg/s |  |  |
| SPSC lock-free | latence, un saut, file seule | p50 251 ns | 235 ns à 256 ns | p99 7,4 µs (de 947 ns à 253 µs) | p99,9 1,79 ms, max 3,77 ms |
| SPSC lock-free | latence, bout en bout (pipeline) | p50 406 ns | 367 ns à 472 ns | p99 64,8 µs (de 2,8 µs à 294 µs) | p99,9 923 µs, max 1,66 ms |
| `std::mutex` + `std::queue` | débit, file seule | 6,08 Mmsg/s | 4,84 à 6,55 Mmsg/s |  |  |
| `std::mutex` + `std::queue` | débit, pipeline 3 threads | 0,76 Mmsg/s | 0,63 à 0,85 Mmsg/s |  |  |
| `std::mutex` + `std::queue` | latence, un saut, file seule | p50 889 ns | 742 ns à 1,1 µs | p99 26,6 µs (de 11,6 µs à 236 µs) | p99,9 1,13 ms, max 3,06 ms |
| `std::mutex` + `std::queue` | latence, bout en bout (pipeline) | p50 1,8 µs | 1,6 µs à 1,9 µs | p99 233 µs (de 22,6 µs à 360 µs) | p99,9 1,31 ms, max 2,43 ms |

**Pricer Monte Carlo : 5 nombres de threads × 2 produits** (`results/mc_pricer.json`)

| threads | européen : temps | speedup | asiatique : temps | speedup | efficacité asiatique |
|--:|--:|--:|--:|--:|--:|
| 1 | 0,776 s | 1,00× | 1,914 s | 1,00× | 1,00 |
| 2 | 0,400 s | 1,94× | 0,990 s | 1,93× | 0,97 |
| 4 | 0,230 s | 3,37× | 0,508 s | 3,77× | 0,94 |
| 8 | 0,211 s | 3,67× | 0,292 s | 6,56× | 0,82 |
| 16 | 0,159 s | 4,89× | 0,306 s | 6,25× | 0,39 |

**Simulateur : 25 configurations, flux pur (persistance 0,50, Santa Fe).** PnL moyen par session (ticks × lots) ; erreur-type de la moyenne de 4 à 12 (500 sessions par ligne)

| Stratégie | 10 µs | 100 µs | 1 ms | 10 ms |
|---|--:|--:|--:|--:|
| sans market maker | 0 | 0 | 0 | 0 |
| AS γ = 0,001 | 2 691 | 2 646 | 2 199 | 877 |
| AS γ = 0,005 | 1 825 | 1 801 | 1 592 | 801 |
| AS γ = 0,02 | 441 | 456 | 528 | 476 |
| naïf, demi-spread 1 | 2 781 | 2 716 | 2 086 | 611 |
| naïf, demi-spread 2 | 3 090 | 3 016 | 2 386 | 849 |
| naïf, demi-spread 3 | 2 541 | 2 467 | 2 056 | 910 |

Détail à L = 100 µs :

| Stratégie | PnL moyen | écart-type | inventaire max moyen (lots) | trades par session |
|---|--:|--:|--:|--:|
| AS γ = 0,001 | 2 646 | 213 | 9,9 | 1 055 |
| AS γ = 0,005 | 1 801 | 147 | 9,1 | 941 |
| AS γ = 0,02 | 456 | 105 | 7,4 | 725 |
| naïf, demi-spread 1 | 2 716 | 185 | 10,0 | 1 910 |
| naïf, demi-spread 2 | 3 016 | 254 | 10,0 | 1 190 |
| naïf, demi-spread 3 | 2 467 | 257 | 10,0 | 722 |

**Simulateur : 25 configurations, flux persistant (persistance 0,75).** PnL moyen par session (ticks × lots) ; erreur-type de la moyenne de 4 à 12 (500 sessions par ligne)

| Stratégie | 10 µs | 100 µs | 1 ms | 10 ms |
|---|--:|--:|--:|--:|
| sans market maker | 0 | 0 | 0 | 0 |
| AS γ = 0,001 | 1 618 | 1 596 | 1 321 | 653 |
| AS γ = 0,005 | 581 | 574 | 554 | 514 |
| AS γ = 0,02 | -254 | -253 | -183 | 197 |
| naïf, demi-spread 1 | 1 896 | 1 843 | 1 387 | 478 |
| naïf, demi-spread 2 | 2 226 | 2 163 | 1 684 | 663 |
| naïf, demi-spread 3 | 1 756 | 1 711 | 1 444 | 719 |

Détail à L = 100 µs :

| Stratégie | PnL moyen | écart-type | inventaire max moyen (lots) | trades par session |
|---|--:|--:|--:|--:|
| AS γ = 0,001 | 1 596 | 190 | 10,0 | 918 |
| AS γ = 0,005 | 574 | 121 | 9,4 | 726 |
| AS γ = 0,02 | -253 | 106 | 7,8 | 470 |
| naïf, demi-spread 1 | 1 843 | 216 | 10,0 | 1 640 |
| naïf, demi-spread 2 | 2 163 | 241 | 10,0 | 974 |
| naïf, demi-spread 3 | 1 711 | 237 | 10,0 | 562 |
<!-- END:variantes -->

</details>

## Pourquoi ce projet

Métiers visés : market making, exécution, plateformes de trading FX. Ce moteur est la brique « vérifiable et mesurée » de cette chaîne : un carnet testé en différentiel, ce que coûte une structure de données par opération (latence séparée du débit), une file inter-threads **correcte** avant d'être rapide, et la question que se pose tout market maker : combien coûte une recotation tardive ? Ce dépôt n'est pas un système de production : pas de réseau, pas de FIX ni d'ITCH, un seul instrument.

## Limites honnêtes

- **Pas de réseau.** Aucune couche gateway, sérialisation (FIX, ITCH), ni file de messages : « latence » veut dire deux choses distinctes selon la partie. Dans le benchmark, c'est le temps CPU d'une opération sur le carnet en mémoire. Dans le simulateur, c'est la **période de recotation** du market maker en temps simulé, sans délai aller-retour, sans course entre agents et sans modèle de file d'attente réseau.
- **Horloge simulée, flux synthétique.** Le simulateur est orienté événements, avec des paramètres de marché (taux, tailles, profondeur) qui ne sont pas calibrés sur des données réelles. Les résultats de market making valent pour ce marché-là ; ils illustrent des mécanismes (latence, calibration, anti-sélection), pas une rentabilité.
- **Mono-instrument**, un seul thread pour le carnet, pas de prévention d'auto-exécution, pas d'autres types d'ordres (stop, iceberg, FOK, post-only), pas de frais ni de rebates.
- **Fenêtre de prix fixe et identifiants denses.** Un prix hors de `[ref - 2048, ref + 2048]` ticks est rejeté ; un vrai moteur recentrerait la fenêtre ou aurait un niveau de débordement. La table `id -> slot` du carnet plat suppose des identifiants séquentiels (bornés à 2^28) ; avec des identifiants épars il faudrait une table de hachage à adressage ouvert.
- **PnL du market maker** : valorisation au mid sans coût de liquidation, sessions de 2 s, taille de cotation et bornes d'inventaire fixées, trois valeurs de gamma. Les écarts entre stratégies sont larges devant l'erreur-type (5 à 12 ticks x lots sur la moyenne) mais ne disent rien de leur robustesse hors de ce marché.
- **Benchmark sur une machine Windows partagée.** Valeurs absolues sensibles à la charge et au cœur (facteur 1,6 observé), rapports plus stables. `rdtsc` suppose un TSC invariant et un x86-64 (hors x86, `include/cpu.hpp` bascule sur `steady_clock` et `yield()` : compilé et exécuté seulement en forçant `-DMARCHE_SANS_X86` sous x86, jamais sur une machine ARM) ; les causes microarchitecturales de l'écart (défauts de cache, allocations) sont argumentées par construction, pas mesurées par compteurs matériels.
- **Pricer** : GBM à volatilité constante, 50 fixings discrets ; le speedup est limité par un CPU hybride et partagé.
- **Concurrence** : la file SPSC est vérifiée par le raisonnement acquire/release, les tests de séquence et ThreadSanitizer en CI Linux ; pas de job ARM, pas de model checking.
- **Tests** : framework maison (compteur d'échecs). Le test différentiel prouve que les deux implémentations sont équivalentes sur les flux testés, pas qu'elles sont correctes sur tous les flux ; les scénarios unitaires et la comptabilité des quantités couvrent le reste.

## Reproduire

Aucune donnée externe à télécharger : le flux d'ordres est synthétique et généré par les exécutables.

```bash
# Linux ou macOS x86-64 (g++ >= 13) ; sous Windows/MinGW voir plus bas
bash build.sh                                   # compile dans build/ (C++20, -O3 -march=native -Wall -Wextra)
./build/test_carnet                             # 134 vérifications + différentiel 1,2 M ops (code de sortie 0 = tout passe)
./build/test_spsc                               # 162 vérifications : file SPSC, 20 M d'éléments, pipeline (~3 s)
{ ./build/test_carnet; ./build/test_spsc; } > results/log_tests.txt   # (optionnel) sortie lue par analyse/vitrine_05.py
./build/bench_carnet [--ops 5000000] [--coeur N]   # écrit results/bench_carnet.json (~20 s) : remplace les résultats de référence
./build/bench_spsc [--reps 5] [--cadence 3e5] [--coeurs 4,6,8 | --sans-epinglage]   # results/bench_spsc.json (~90 s)
./build/simulateur [--sessions 500] [--graine S] [--threads N] [--duree 2.0]   # sessions.csv, trajectoire.csv, serie_mid.csv
./build/mc_pricer  [--paires-euro N] [--paires-asia N] [--reps R]              # écrit results/mc_pricer.json
pip install -r requirements.txt                 # numpy, pandas, matplotlib (figures seulement)
python analyse/analyse.py                       # figures fig1 à fig7 + results/resume_pnl.csv
python analyse/vitrine_05.py                    # figure héro, fig8, toutes_variantes_mm + blocs générés de ce README
```

Avec CMake (même cibles, plus `ctest`) :

```bash
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release   # -DNATIVE=OFF : binaire portable
cmake --build build-cmake -j
ctest --test-dir build-cmake --output-on-failure       # test_carnet + test_spsc
```

Variables de `build.sh` (toutes facultatives) : `CXX` (compilateur, défaut `g++`), `NATIVE=0` (pas de `-march=native`, binaire portable), `CXXEXTRA` (drapeaux en plus, ex. `CXXEXTRA="-fsanitize=address,undefined"` ou `"-fsanitize=thread -DATTENTE_COOPERATIVE"` sous Linux).
Sous Windows/MinGW (Git Bash), `build.sh` lie en statique (pas de DLL à trouver) et les exécutables ont l'extension `.exe` : `CXX=/chemin/vers/g++.exe bash build.sh`, puis `./build/test_carnet.exe`. Les sanitizers n'existent pas dans ce MinGW : ils tournent en CI.
Les intrinsèques x86 (`rdtsc`, `_mm_pause`) sont isolés dans `include/cpu.hpp`, gardés par `#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)` ; ailleurs le code se replie sur `std::this_thread::yield()` et `steady_clock`. Le benchmark de latence n'a de sens que sur x86-64.

Intégration continue (`.github/workflows/tests.yml`, ubuntu-latest, g++ ≥ 13) : build et tests avec et sans `-march=native` ; les mêmes tests sous AddressSanitizer + UBSan ; la file SPSC et le pipeline sous ThreadSanitizer ; build CMake et `ctest`. Les benchmarks ne sont pas rejoués en CI (une machine virtuelle partagée ne donnerait pas de latences exploitables). Sur une machine à moins de 4 cœurs, `build.sh` compile `test_spsc` avec `-DATTENTE_COOPERATIVE` (les threads en attente cèdent le processeur) : sans cela, trois threads en attente active sur deux cœurs n'avancent pas.

`.gitignore` : `build/` et `build-cmake/` sont exclus. Git ne filtre pas par taille : tout CSV de plus de 5 Mo doit être écrit dans `results/gros/` (ou nommé `*.gros.csv`) pour être ignoré ; aujourd'hui les trois CSV font moins de 2 Mo (`find results -name '*.csv' -size +5M` doit rester vide).

## Structure du dépôt

Un `README.md` dans chaque dossier décrit ses fichiers.

```
.
  build.sh, CMakeLists.txt      compilation : script bash ou CMake (CXX, NATIVE, CXXEXTRA)
  requirements.txt              numpy, pandas, matplotlib (figures uniquement)
  .github/workflows/tests.yml   CI : build + tests, ASan/UBSan, ThreadSanitizer, CMake
  include/                      carnet.hpp (types, deux carnets), util.hpp (SplitMix64), spsc.hpp (file lock-free),
                                file_mutex.hpp (référence mutex), pipeline.hpp (feed -> carnet -> stratégie),
                                cpu.hpp (rdtsc / pause x86 et repli portable)
  src/                          carnet.cpp (les deux implémentations), simulateur.cpp (multi-agents + MM), mc_pricer.cpp
  tests/                        test_carnet.cpp (134 vérifications), test_spsc.cpp (162 vérifications)
  bench/                        bench_carnet.cpp (latence rdtsc), bench_spsc.cpp (SPSC contre mutex)
  analyse/                      analyse.py (fig1-7), vitrine_05.py (héro, fig8, variantes, blocs du README)
  results/                      JSON, CSV et journaux produits par les exécutables
  figures/                      PNG (130 dpi) produits par analyse/
  docs/                         conception, résultats détaillés, positionnement
```

## Licence

MIT, voir [`LICENSE`](LICENSE).
