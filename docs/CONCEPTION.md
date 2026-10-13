# Conception : architecture et choix

Détails de conception du moteur. Les résultats chiffrés sont dans le [README](../README.md) et dans [RESULTATS_DETAILLES.md](RESULTATS_DETAILLES.md).

## Architecture

```
                      +------------------------------ include/carnet.hpp, src/carnet.cpp ---------------------------+
                      |  API commune (par template, sans virtuel) :                                                  |
                      |  ajouter_limite | annuler | modifier | ordre_marche  ->  journal de Trade (maker, taker, prix, qte)|
                      |                                                                                              |
                      |  (a) CarnetMap                         (b) CarnetPlat                                        |
                      |   std::map<prix, std::list<ordre>>      niveaux[prix - base] (16 o) + bitmap des niveaux     |
                      |   unordered_map<id, iterateur>          pool d'ordres contigu, chaînage par indices          |
                      |                                         table id -> slot (indexation directe)                |
                      +-------------+--------------------------------+---------------------+-------------------------+
                                    |                                |                     |
                  tests/test_carnet.cpp                bench/bench_carnet.cpp        src/simulateur.cpp  (utilise (b))
                  - scénarios (FIFO, partiel,          - flux 70/25/5 généré une     - traders ZI Poisson (limites, marché,
                    croisement, balayage, modif)         fois, rejoué sur (a) et (b)     annulations par ordre vivant)
                  - différentiel (a) = (b) sur         - latence/op par rdtsc          - market maker AS  /  naïf  /  aucun
                    1,2 M ops + comptabilité           - débit hors chronomètre        - 500 sessions Monte Carlo, N threads
                         |                                      |                              |
                         v                                      v                              v
                    code retour 0/1               results/bench_carnet.json      results/sessions.csv, trajectoire.csv,
                                                                                  serie_mid.csv, simulateur_meta.json

   src/mc_pricer.cpp (indépendant) : GBM, antithétiques, 1 générateur par thread  ->  results/mc_pricer.json

   analyse/analyse.py : lit results/*  ->  figures/fig1..fig7 (PNG, 130 dpi) + results/resume_pnl.csv
```

## Choix de conception

**Prix en ticks entiers.** Un prix est un `int32_t` en nombre de ticks, jamais un `double` : l'égalité de deux niveaux de prix doit être exacte (sinon deux ordres « au même prix » se retrouvent dans deux niveaux), et l'indexation d'un tableau par le prix devient une simple soustraction.

**Priorité prix-temps.** Meilleur prix d'abord, puis ancienneté (FIFO) à l'intérieur d'un niveau. Un trade se fait toujours au prix de l'ordre au repos (maker). Une baisse de quantité garde la place dans la file ; une hausse de quantité la perd (l'ordre repasse en fin de file) ; un changement de prix est une annulation suivie d'une réinsertion, qui peut croiser le carnet. Un ordre au marché balaie les niveaux et son reliquat est abandonné (IOC).

**(a) `std::map` + `std::list`, la version de manuel.** Un arbre rouge-noir de niveaux, une liste chaînée d'ordres par niveau, une table de hachage `id -> itérateur` pour annuler en O(1) sur la liste. Correct, lisible, et c'est la référence honnête à battre.

**(b) tableau plat indexé par tick.** Un `std::vector` de niveaux de 16 octets (tête, queue, quantité totale) indexé par `prix - base`, une bitmap par côté qui permet de retrouver le meilleur niveau suivant avec `countl_zero` / `countr_zero` quand un niveau se vide, un **pool d'ordres contigu** où la file FIFO d'un niveau est une liste chaînée par *indices* (pas par pointeurs), une free-list pour recycler les slots, et une table `id -> slot` indexée directement par l'identifiant (les identifiants sont séquentiels, comme ceux d'un gateway).

**Pourquoi (b) est plus rapide** (par construction ; je n'ai pas mesuré les caches avec des compteurs matériels) :
- *Localité de cache* : les niveaux voisins sont voisins en mémoire, et le touch (là où se passent presque toutes les opérations) tient dans quelques lignes de cache ; avec `std::map` chaque niveau est un nœud d'arbre alloué ailleurs et chaque ordre un nœud de liste, d'où une chaîne de défauts de cache par opération.
- *Pas d'allocation dans la boucle chaude* : slots réservés d'avance et recyclés, journal de trades réutilisé (on le vide, la capacité reste), tables dimensionnées au départ. (a) fait un `new`/`delete` de nœud par ajout, par exécution et par niveau créé.
- *Pas de hachage* : l'annulation passe par un accès direct `pos[id]`, au lieu de hacher puis suivre un pointeur. C'est l'opération où l'écart est le plus grand (11x sur le p50, voir plus bas).
- *Meilleur prix en O(1)* : le meilleur niveau est mémorisé et, s'il se vide, la bitmap donne le suivant sans balayer.

**Contrat commun et fenêtre de prix.** Les deux carnets reçoivent la même `Config` (prix de référence, demi-largeur, tailles de réserve) et rejettent un prix hors de `[prix_ref - demi, prix_ref + demi]` : le plat en a besoin, et l'imposer au map rend les deux strictement équivalents (donc testables l'un contre l'autre sans exception). Un identifiant déjà vivant, une quantité nulle ou négative, un identifiant supérieur à 2^28 sont rejetés.
