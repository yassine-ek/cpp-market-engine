# bench/ : benchmarks de latence et de débit

Les benchmarks écrivent leurs résultats dans `results/`. Ils exigent un x86-64 (`rdtsc`) ; l'épinglage de cœur et la priorité élevée ne sont appliqués que sous Windows. **Ne pas les lancer en CI** : une machine virtuelle partagée ne donne pas de latences exploitables.

| Fichier | Mesure | Produit |
|---|---|---|
| `bench_carnet.cpp` | Latence par opération (p50 / p99 / p99,9 / max, histogramme logarithmique) et débit de `CarnetMap` contre `CarnetPlat` sur un même flux de 5 M d'opérations (70 % ajouts, 25 % annulations, 5 % ordres au marché), chauffe de 500 k, 3 répétitions alternées. Horloge `rdtsc` encadrée de `lfence`, overhead de mesure mesuré et non soustrait. Options : `--ops N`, `--coeur N`. | `results/bench_carnet.json` |
| `bench_spsc.cpp` | File SPSC contre `std::mutex` + `std::queue` : débit de la file seule, latence d'un saut à cadence fixe, débit et latence bout en bout du pipeline feed → carnet → stratégie ; 5 répétitions alternées, médiane publiée, p99 de chaque répétition conservé. Options : `--reps`, `--cadence`, `--coeurs a,b,c` ou `--sans-epinglage`. | `results/bench_spsc.json` |
