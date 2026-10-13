# include/ : en-têtes (header seul)

Tout le code partagé est ici, sans dépendance externe. Les deux carnets ont la **même API sans héritage virtuel** : les tests et les benchmarks les manipulent par `template`, pour que la mesure ne paie aucun appel indirect.

| Fichier | Rôle |
|---|---|
| `carnet.hpp` | Types (`Prix` en ticks entiers, `Id`, `Qte`, `Trade`, `Config`) et déclaration des deux carnets à priorité prix-temps : `CarnetMap` (`std::map` + `std::list` + table de hachage) et `CarnetPlat` (tableau indexé par tick, bitmap des niveaux, pool d'ordres contigu). Les implémentations sont dans `src/carnet.cpp`. |
| `util.hpp` | `splitmix64` : dérive des graines décorrélées à partir d'une graine et d'un indice (une graine par thread ou par session). |
| `spsc.hpp` | `FileSpsc<T>` : file lock-free à un producteur et un consommateur, capacité en puissance de 2, deux compteurs monotones sur des lignes de cache séparées, `acquire` / `release`. |
| `file_mutex.hpp` | `FileMutex<T>` : file de référence `std::queue` + `std::mutex`, même API et même capacité que `FileSpsc`, pour comparer à charge identique. |
| `pipeline.hpp` | Pipeline à 3 threads feed → carnet → stratégie, générique sur le type de file ; génération de la séquence d'ordres, empreinte de la stratégie, épinglage de cœur (Windows), attente active (`attendre()`, coopérative sous `-DATTENTE_COOPERATIVE`). |
