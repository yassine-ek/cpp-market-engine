# Résultats détaillés

Mesures, méthode et lecture de chaque figure. Le résumé chiffré et les hypothèses de mesure sont dans le [README](../README.md) ; la conception est dans [CONCEPTION.md](CONCEPTION.md). Tous les chiffres viennent de `results/*.json` et `results/*.csv`.

## Résultats détaillés

### Tests du carnet (`tests/test_carnet.cpp`) : 134 vérifications, 0 échec (sortie : `results/log_tests.txt`)

Scénarios sur chaque implémentation : FIFO respecté, exécution partielle avec conservation de la priorité du reliquat, annulation (double annulation, id inconnu), croisement de carnet (prix du maker, reliquat qui repose), ordre au marché qui balaie plusieurs niveaux puis dépasse le carnet, modification (baisse, hausse, changement de prix qui croise, prix hors fenêtre), rejets, bords exacts de la fenêtre de prix (±2048 ticks acceptés, ±2049 refusés, aucun accès hors tableau).

Test différentiel : 1 000 000 d'opérations aléatoires (ajouts passifs et agressifs, prix hors fenêtre, annulations d'ordres vivants ou morts, ordres au marché, modifications), puis 200 000 avec une autre graine. À chaque opération on compare le retour, **la liste exacte des trades dans l'ordre**, les meilleurs prix, les quantités totales, le nombre d'ordres, et toutes les 2 000 opérations la profondeur sur 8 niveaux. Résultat : 0 désaccord, 500 965 trades sur le premier flux. Une comptabilité indépendante des carnets (quantité totale au repos = ce qu'on y a mis moins ce qui est parti) et l'absence de croisement du carnet sont vérifiées à chaque pas.

Je n'ai pas seulement vérifié que les tests passent : j'ai cassé volontairement le carnet plat de deux façons (une hausse de quantité qui garde sa priorité ; un prix de trade décalé d'un tick) et la suite échoue dans les deux cas (3 et 7 échecs).

### Benchmark de latence (`results/bench_carnet.json`)

Flux de 5 M d'opérations mesurées après 500 k de chauffe : 70 % ajouts, 25 % annulations, 5 % ordres au marché. Le flux est généré **une fois** en pilotant un carnet de référence (les annulations visent des ordres réellement vivants, les prix sont relatifs au touch, 10 % des limites sont marchables), puis rejoué à l'identique sur (a) et (b) ; les deux produisent le même nombre de trades (2,68 M sur l'ensemble du flux) et le même volume. Le carnet reste autour de 50 000 ordres vivants (la taille des ordres au marché est asservie à ce nombre ; sans cela 70 % d'ajouts contre 25 % d'annulations le feraient croître sans fin).

| 5 M opérations | p50 | p99 | p99.9 | max | débit |
|---|---|---|---|---|---|
| (a) `std::map` + `std::list` | 214 ns | 1 709 ns | 7 600 ns | 291 µs | 3,26 Mops/s |
| (b) tableau plat | 42 ns | 290 ns | 449 ns | 254 µs | 33,4 Mops/s |
| rapport (a)/(b) | 5,1x | 5,9x | 16,9x | | 10,2x (débit) |

Par type d'opération (p50 / p99) : ajout 183 / 493 ns (a) contre 38 / 90 ns (b) ; annulation 680 / 1 168 ns contre 62 / 347 ns (11x sur le p50) ; ordre au marché 542 / 9 336 ns contre 105 / 467 ns. Le p99 global de (a) est surtout porté par les ordres au marché, qui balaient plusieurs niveaux et libèrent plusieurs nœuds.

Méthode et précautions :
- *Horloge.* `std::chrono::steady_clock` n'a ici qu'une granularité de **100 ns** (QueryPerformanceCounter sous Windows), plus grosse que l'opération mesurée : la latence par opération est lue avec le compteur TSC (`rdtsc`, calibré sur `steady_clock`, soit 2,995 GHz), encadré de `lfence`. Le coût de la mesure elle-même (15,4 ns) est mesuré et **n'est pas soustrait** : les chiffres ci-dessus sont bruts. Le débit vient d'une passe séparée, sans chronomètre par opération, mesurée avec `steady_clock`.
- *Répétitions.* 3 répétitions alternées (a, b, a, b, ...) ; on publie la répétition de p50 médian et le débit médian (débits (a) : 3,23 / 3,53 / 3,26 Mops/s ; (b) : 28,0 / 33,8 / 33,4 Mops/s).
- *Épinglage.* Le thread est épinglé sur un cœur et la priorité du processus est relevée (Windows). Le run publié est épinglé sur le cœur logique 14 (`--coeur 14`) après avoir constaté que d'autres cœurs (2, 4, 6) étaient visiblement plus lents ou plus chargés.
- *Variabilité réelle.* Sur cette machine partagée, d'autres exécutions (complètes, ou à 1 M d'opérations sur d'autres cœurs) ont donné p50 (a) 313-338 ns et p50 (b) 66-68 ns (cœur plus lent ou plus chargé : l'overhead d'horloge passe alors de 15 à 31 ns). Les valeurs absolues bougent d'un facteur 1,6 ; **le rapport (a)/(b) sur le p50 reste dans 4,7-5,1x**. Les `max` (centaines de µs) sont des interruptions de l'ordonnanceur Windows, pas un comportement du carnet.

![Histogrammes des latences par opération, std::map contre tableau plat](../figures/fig1_histogramme_latences.png)

**Comment lire ce graphique.** Chaque barre compte les opérations dont la latence tombe dans une classe logarithmique (axes en log : une barre 100 fois plus basse est tout de même visible). Plus la masse est à gauche, plus l'opération est rapide ; les traits verticaux marquent le p50 (tirets) et le p99 (pointillés). Le tableau plat concentre l'essentiel de ses opérations entre 25 et 100 ns ; `std::map` s'étale sur trois décades et traîne une longue queue (allocations, parcours d'arbre). Les quelques opérations à 100-300 µs, présentes dans les deux histogrammes, sont des interruptions de l'ordonnanceur Windows.

![Percentiles, p50 par type d'opération et débit des deux carnets](../figures/fig2_comparaison_carnets.png)

**Comment lire ce graphique.** À gauche, les trois percentiles globaux (échelle log) : l'écart passe de 5x au p50 à 17x au p99,9. Au milieu, le p50 par type d'opération : l'annulation est l'opération où l'écart est le plus grand (accès direct `pos[id]` contre hachage puis pointeur). À droite, le débit mesuré dans une passe séparée, sans chronomètre par opération.

### Pricer Monte Carlo (`results/mc_pricer.json`)

GBM, S0 = 100, K = 100, T = 1, r = 0,02, sigma = 0,2. Un `mt19937_64` par thread (graines dérivées par SplitMix64), variables antithétiques (le couple (Z, -Z) est traité comme une observation, l'erreur-type est calculée sur les couples), accumulation locale puis réduction : pas de verrou ni de faux partage.

| | prix | erreur-type | IC95 | référence |
|---|---|---|---|---|
| Call européen (20 M couples) | 8,91612 | 0,00166 | [8,91285 ; 8,91938] | Black-Scholes 8,91604 : écart +0,05 erreur-type |
| Call asiatique arithmétique (1 M couples, 50 fixings) | 5,12250 | 0,00407 | [5,11452 ; 5,13047] | |
| Validation : asiatique **géométrique** (mêmes trajectoires) | 4,93494 | 0,00395 | | forme fermée 4,94185 : écart -1,75 erreur-type |

Le critère demandé (call européen à moins de 3 erreurs-types de Black-Scholes) est largement tenu. Black-Scholes ne valide que l'européen ; l'asiatique est validé indirectement par le géométrique calculé sur les mêmes trajectoires, dont le prix a une forme fermée (je l'ai testé avec 3 autres graines : écarts de +0,6, +0,1 et +0,4 erreur-type, donc pas de biais visible). L'ordre `géométrique <= arithmétique <= européen` est vérifié. L'exécutable renvoie un code d'erreur si l'un des écarts dépasse 3 erreurs-types.

Speedup (meilleur de 5 répétitions, après chauffe) :

| threads | 1 | 2 | 4 | 8 | 16 |
|---|---|---|---|---|---|
| européen | 1,00x (0,776 s) | 1,94x | 3,37x | 3,67x | 4,89x |
| asiatique | 1,00x (1,914 s) | 1,93x | 3,77x | 6,56x | 6,25x |

La machine expose 22 threads logiques mais c'est un processeur **hybride** (cœurs P, cœurs E, SMT) partagé avec d'autres tâches : le speedup est proche de l'idéal jusqu'à 4 threads puis plafonne ; l'européen, qui fait peu de calcul par nombre aléatoire, sature plus tôt que l'asiatique. Sur plusieurs exécutions, le speedup à 16 threads a varié entre 4,7x et 7,3x. Le prix publié vient de l'exécution à 16 threads ; il dépend légèrement du nombre de threads (autres flux aléatoires), tous statistiquement valides. 

![Speedup du pricer Monte Carlo selon le nombre de threads](../figures/fig7_speedup_pricer.png)

**Comment lire ce graphique.** Axes en log base 2 ; la diagonale pointillée est le speedup idéal. Le pricer suit l'idéal jusqu'à 4 threads, puis s'en écarte : processeur hybride (cœurs P et E, SMT) partagé avec d'autres tâches. L'européen, qui fait peu de calcul par nombre aléatoire, sature avant l'asiatique.

### Simulateur multi-agents et market maker (`results/sessions.csv`)

**Le marché.** Traders « zero-intelligence » à la Santa Fe : des ordres limites arrivent selon un processus de Poisson (8 000 /s), placés entre 1 et 40 ticks sous le meilleur ask (achat) ou au-dessus du meilleur bid (vente), jamais marchables ; des ordres au marché arrivent à 1 500 /s (taille 1 + Géom(0,4) lots) ; chaque ordre vivant est annulé au taux de 8 /s. Une session = 1 s de chauffe sans market maker, puis 2 s avec. L'horloge est simulée en microsecondes ; environ 15,6 événements ZI par milliseconde (nombre d'ordres vivants moyen mesuré : 760), soit environ 47 000 événements ZI par session de 3 s.

**Deux régimes de flux.** `persistance = 0,50` est le Santa Fe pur (le sens de chaque ordre au marché est tiré à pile ou face). `persistance = 0,75` donne au sens des ordres au marché une mémoire (probabilité 0,75 d'avoir le même sens que le précédent) : c'est un fait observé sur les vrais marchés, et la **seule source d'anti-sélection** possible ici puisque des traders ZI purs n'ont aucune information. Les deux régimes sont simulés ; le premier est le résultat principal, le second teste la robustesse.

**Les market makers** recotent toutes les L microsecondes simulées, à partir de la vue du marché *sans leurs propres ordres* :
- **Avellaneda-Stoikov** : prix de réservation r = s - q gamma sigma^2 (T - t), demi-spread = [gamma sigma^2 (T - t) + (2/gamma) ln(1 + gamma/k)] / 2, cotes arrondies au tick vers l'extérieur, 2 lots par côté, borne d'inventaire de 10 lots (on ne cote plus du côté qui l'aggraverait), cotes toujours passives. sigma^2 et k sont **calibrés** par une session de 300 s sans market maker : k par régression de ln P(profondeur atteinte par un ordre au marché >= delta) sur delta, sigma^2 sur la variance par seconde du mid à l'horizon moyen de la session (T/2 = 1 s). Régime pur : sigma^2 = 152,1 ticks^2/s, k = 0,503 /tick, spread ZI moyen de 4,08 ticks.
- **Naïf à spread fixe** : floor(mid) - h et ceil(mid) + h, h = 1, 2 ou 3 ticks (entiers, car un demi-spread réel serait écrasé par l'arrondi au tick), mêmes bornes d'inventaire.
- **Sans market maker** (référence).

Grille : L dans {10 µs, 100 µs, 1 ms, 10 ms}, gamma dans {0,001 ; 0,005 ; 0,02} (trois valeurs, fixées avant de lancer, pas de grille fine), 500 sessions par configuration, 25 configurations par régime : 25 000 sessions, 12 s sur 22 threads. La graine d'une session ne dépend que de son indice (SplitMix64) : toutes les stratégies partent des mêmes aléas (comparaison appariée) et le fichier `sessions.csv` est **identique quel que soit le nombre de threads** (vérifié). Le PnL est cash + inventaire x mid ZI final, en ticks x lots, sans coût de liquidation ni frais.

**PnL moyen par session (ticks x lots), régime pur, 500 sessions, erreur-type de la moyenne de 5 à 12 :**

| L | AS g=0,001 | AS g=0,005 | AS g=0,02 | naïf h=1 | naïf h=2 | naïf h=3 |
|---|---|---|---|---|---|---|
| 10 µs | 2 691 | 1 825 | 441 | 2 781 | 3 090 | 2 541 |
| 100 µs | 2 646 | 1 801 | 456 | 2 716 | 3 016 | 2 467 |
| 1 ms | 2 199 | 1 592 | 528 | 2 086 | 2 386 | 2 056 |
| 10 ms | 877 | 801 | 476 | 611 | 849 | 910 |

À L = 100 µs : AS gamma = 0,005 gagne en moyenne 1 801 (écart-type 147, inventaire maximal moyen 9,1 lots, 941 trades) ; le naïf h = 2 gagne 3 016 (écart-type 254, 10,0 lots, 1 190 trades) ; AS gamma = 0,02 gagne 456 (écart-type 105, 7,4 lots).

**Ce que j'en lis :**
- **La latence coûte cher, de façon monotone sauf pour gamma élevé.** De 10 µs à 10 ms, le PnL de l'AS gamma = 0,005 passe de 1 825 à 801 (-56 %) et celui du naïf h = 2 de 3 090 à 849 (-73 %). Entre 10 µs et 100 µs la différence est négligeable, elle devient nette à 1 ms (environ 16 événements ZI entre deux recotations) et massive à 10 ms : un market maker qui laisse ses cotes vieillir est moins souvent au bon endroit (trades divisés par 2,5 à 3) et plus souvent exécuté quand le marché a bougé contre lui.
- **L'AS ne bat pas le naïf bien réglé en PnL moyen dans ce simulateur.** À L = 100 µs, le meilleur AS (gamma = 0,001) gagne 2 646 contre 3 016 pour le naïf h = 2 ; le naïf h = 1, qui cote à l'intérieur du spread des ZI, a même le meilleur rapport moyenne / écart-type (14,6 contre 12,4). Le naïf atteint la borne d'inventaire de 10 lots dans presque toutes les sessions sans en payer le prix : dans un marché ZI sans information privée, porter de l'inventaire ne coûte presque rien, et la valorisation au mid ignore le coût de liquidation. Ce que l'AS fait bien : à gamma = 0,02 il réduit l'inventaire maximal moyen (7,4 lots contre 10) et la dispersion du PnL (écart-type 105 contre 254), au prix d'un PnL moyen bien plus bas. **Ce n'est pas un verdict sur l'AS en général** : le simulateur n'a ni information privée ni coût de liquidation, deux ingrédients qui avantagent la gestion d'inventaire.
- **Flux persistant (persistance 0,75) : le PnL baisse partout et l'anti-sélection apparaît.** À L = 100 µs, AS gamma = 0,005 tombe de 1 801 à 574, naïf h = 2 de 3 016 à 2 163, et l'AS gamma = 0,02 devient **négatif** (-253 ; les fills ne surviennent plus que lors de mouvements forts, c'est-à-dire exactement quand ils sont toxiques). Pour ce gamma, le PnL *monte* avec L (-254 à 10 µs, +197 à 10 ms) : recoter plus rarement évite de courir après un mid qui revient.
- **La calibration de sigma^2 compte beaucoup.** Le mid des ZI revient vers sa valeur après chaque ordre au marché (le carnet se remplit) : sa variance par seconde vaut 3 998 ticks^2/s à 1 ms, 391 à 100 ms, 176 à 1 s, 154 à 2 s. Ma première calibration à 100 ms (sigma^2 d'environ 380 à 400 ticks^2/s) faisait gagner à l'AS gamma = 0,005 environ 980 à L = 100 µs contre 1 801 une fois sigma^2 calibré à l'horizon de (T - t), parce que le skew d'inventaire était trop fort. Je garde la calibration à l'horizon moyen, qui est celle que le modèle suppose, et je la signale parce qu'elle change la lecture.

![PnL du market maker selon la latence, boîtes par configuration, deux régimes de flux](../figures/fig3_pnl_vs_latence.png)

**Comment lire ce graphique.** Une boîte = 500 sessions d'une même configuration (trait central = médiane, moustaches = étendue). Colonne de gauche : Avellaneda-Stoikov pour trois aversions au risque γ ; colonne de droite : market maker naïf pour trois demi-spreads. Ligne du haut : flux pur ; ligne du bas : flux persistant. On lit la perte de PnL quand la période de recotation L passe de 10 µs à 10 ms, et le passage sous zéro de l'AS à γ = 0,02 quand le flux est persistant.

![Distribution du PnL à L = 100 µs et plan rendement / risque](../figures/fig4_pnl_as_vs_naif.png)

**Comment lire ce graphique.** À gauche, distributions du PnL de session à L = 100 µs (traits pleins : flux pur ; tirets : flux persistant). À droite, un point par configuration : PnL moyen en ordonnée, écart-type en abscisse, forme = période de recotation. Les points en haut à gauche sont les meilleurs (plus de PnL pour moins de dispersion) ; le naïf est en haut à droite : il gagne plus, avec plus de dispersion.

![Une session exemple : mid, cotes du market maker, inventaire, PnL](../figures/fig5_trajectoire.png)

**Comment lire ce graphique.** Une seule session de 2 s (Avellaneda-Stoikov, L = 100 µs, γ = 0,005). Panneau 1 : le mid des traders (noir), le spread du carnet (gris) et les deux cotes du market maker (bleu : achat, orange : vente). Panneau 2 : zoom de 100 ms où l'on voit les cotes décalées par l'inventaire. Panneau 3 : inventaire en lots, borné à ±10. Panneau 4 : PnL mark-to-market de la session.

**Faits stylisés du simulateur** (100 s de marché sans market maker, mid échantillonné toutes les 1 ms) : les rendements ont des **queues épaisses** (kurtosis en excès 3,9) ; l'autocorrélation des rendements est négative au premier décalage (-0,15 : le carnet se remplit après un balayage) ; l'autocorrélation des rendements en valeur absolue est positive mais **de courte mémoire** (0,14 au décalage 1 ms, 0,03 à 10 ms, nulle à 100 ms), donc un clustering de volatilité faible et pas de longue mémoire. Le modèle reproduit les queues épaisses ; il ne reproduit pas la persistance de la volatilité observée sur les marchés réels.

![Faits stylisés du simulateur : queues épaisses et autocorrélations](../figures/fig6_faits_stylises.png)

**Comment lire ce graphique.** À gauche, l'histogramme des variations du mid standardisées (échelle log) comparé à la loi normale : les queues du simulateur sont plus épaisses que celles de la normale. À droite, l'autocorrélation des rendements (orange, négative au premier décalage) et de leur valeur absolue (bleu, positive mais qui retombe sous la bande de bruit blanc en quelques dizaines de ms) : le modèle ne produit pas de volatilité à longue mémoire.

## File SPSC lock-free et pipeline

Fichiers : `include/spsc.hpp` (la file), `include/file_mutex.hpp` (référence `std::mutex` + `std::queue`), `include/pipeline.hpp` (pipeline), `tests/test_spsc.cpp`, `bench/bench_spsc.cpp`, `results/bench_spsc.json`.

**Pipeline.** Trois threads : *feed* (rejoue une séquence d'ordres déterministe) -> file 1 -> *carnet* (applique chaque ordre au `CarnetPlat` et publie le top-of-book après **chaque** message) -> file 2 -> *stratégie* (compte les changements de mid et les spreads serrés, calcule une empreinte de tout ce qu'elle a reçu). Le même code tourne avec n'importe quelle file (template), ce qui rend la comparaison SPSC / mutex à charge strictement identique.

**Design de la file** (`FileSpsc<T>`, un producteur, un consommateur) :
- *Capacité en puissance de 2, compteurs monotones.* Deux compteurs 64 bits qui ne font que croître ; la case d'un élément est `compteur & masque`. Vide = `tete == tail`, pleine = `tail - tete == capacité` : aucune case sacrifiée, et le wrap-around se réduit à un ET binaire (le test le vérifie sur plus de 1 000 tours de file).
- *Pourquoi `acquire`/`release` suffit.* Chaque compteur a un seul écrivain, donc aucune course d'écriture à arbitrer ; il reste à garantir la visibilité. Le producteur écrit la case puis publie `tail` en `release` ; le consommateur lit `tail` en `acquire` : s'il voit le nouveau `tail`, il voit le contenu de la case. Symétriquement le consommateur lit la case puis publie `tete` en `release` ; le producteur lit `tete` en `acquire` avant de réutiliser la case. Pas besoin de `seq_cst` (aucun raisonnement ne repose sur un ordre total entre les deux compteurs) ; la lecture de son propre compteur est `relaxed`.
- *Faux partage.* Les deux compteurs sont écrits par des cœurs différents : chacun est seul sur sa ligne de cache de 64 octets (`alignas(64)`). Sinon chaque écriture invaliderait la ligne chez l'autre cœur, même sans donnée commune. J'ai fixé 64 en dur plutôt que `std::hardware_destructive_interference_size`, que GCC signale par un avertissement dans un en-tête. Je n'ai **pas** mesuré la version sans `alignas` : ce choix est justifié par construction, pas par une expérience.
- *Cache de l'index opposé.* Le producteur garde une copie du `tete` du consommateur et ne relit le vrai (une ligne de cache qui change de cœur) que lorsque la copie dit « pleine » ; idem côté consommateur avec `tail`. La copie est toujours en retard, donc prudente : elle peut dire « plein » à tort, jamais « libre » à tort. Même remarque : non mesuré isolément.

**Tests** (`tests/test_spsc.cpp`, 162 vérifications, 0 échec ; la suite du carnet reste à 134/0) : FIFO, vide, plein (la 9e insertion d'une file de 8 est refusée), wrap-around avec paquets de tailles variées, type composite de 24 octets ; deux threads sur **20 millions** d'éléments (capacité 1024) : séquence exacte, 0 perte, 0 doublon, 0 élément hors ordre, rien en trop à la fin ; stress sur une file de capacité 2 (3 M) et sur la file mutex (2 M) ; pipeline sur 2 M d'ordres (1,4 M trades) avec capacité 1024 et capacité 4 (forte contre-pression) et avec la file mutex : carnet final (meilleurs prix, quantités, nombre d'ordres, 40 niveaux de profondeur par côté), nombre de trades et empreinte de la stratégie **identiques** à l'exécution mono-thread sur la même séquence. Test de mutation : en supprimant le test « file pleine » de `try_push`, la suite échoue (21 échecs). Limite de ces tests : ils tournent sur x86-64, dont le modèle mémoire (TSO) est plus fort que celui qu'impose C++ ; une erreur d'ordonnancement mémoire (`release` oublié) ne s'y verrait pas forcément. La correction repose sur le raisonnement ci-dessus, pas sur ces tests seuls ; ThreadSanitizer, exécuté en CI Linux, complète le dispositif.

**Mesures** (`results/bench_spsc.json`, médiane de 5 répétitions alternées SPSC / mutex, 3 threads épinglés sur les cœurs logiques 4, 6, 8 par `SetThreadAffinityMask`, priorité du processus relevée, `-O3 -march=native`, capacité 1024 par file) :

| | SPSC lock-free | `std::mutex` + `std::queue` | rapport |
|---|---|---|---|
| Débit, file seule (20 M entiers 64 bits) | 99,7 Mmsg/s | 6,1 Mmsg/s | 16x |
| Débit, pipeline 3 threads (4 M ordres) | 6,9 Mmsg/s | 0,76 Mmsg/s | 9x |
| Latence d'un saut, file seule, p50 / p99 / p99.9 | 251 ns / 7,4 µs / 1,8 ms | 889 ns / 26,6 µs / 1,1 ms | p50 : 3,5x |
| Latence bout en bout (feed -> stratégie, 2 files + carnet), p50 / p99 / p99.9 | 406 ns / 65 µs / 0,92 ms | 1 844 ns / 233 µs / 1,3 ms | p50 : 4,5x |

Les latences sont mesurées à **cadence fixe de 300 000 msg/s** (1 M de messages, 10 % de chauffe écartés) : l'émetteur estampille l'heure prévue d'émission avec `rdtsc`, le récepteur lit `rdtsc` à l'arrivée. Je n'ai pas pu garder 1 Mmsg/s : le pipeline mutex ne soutient que 0,6-0,85 Mmsg/s, donc sa file se remplit et la « latence » mesurée n'est plus que le temps de remplissage (mesuré lors d'un premier essai : p50 de 18 à 590 ms). Les pipelines SPSC et mutex donnent à chaque répétition la même empreinte de stratégie que la référence mono-thread.

Lecture honnête :
- Le **p50** est stable d'une répétition à l'autre (SPSC 367-472 ns, mutex 1 622-1 924 ns pour le pipeline) ; le gain du lock-free est net là-dessus. Le débit de 100 Mmsg/s de la file seule est un débit en rafale entre deux threads qui ne font rien d'autre, pas un débit applicatif : le pipeline complet, où le carnet a un coût réel, tombe à 6,9 Mmsg/s, borné par le thread carnet (à comparer aux 33 Mops/s du carnet plat seul sur son flux de benchmark ; le flux n'est pas le même, donc ce n'est qu'un ordre de grandeur).
- Les **queues** (p99, p99.9, max) sont dominées par l'ordonnanceur Windows, pas par les files : sur une machine non isolée avec 22 threads matériels, des interruptions de centaines de µs surviennent, et le p99 varie de 0,9 µs à 253 µs d'une répétition à l'autre pour la même file (voir `p99_par_repetition` dans le JSON). Les p99 / p99.9 du tableau sont des médianes de valeurs très dispersées : ne pas en tirer d'écart SPSC / mutex dans les queues. Seuls le p50 et le débit permettent de les départager ici.
- `rdtsc` lu sur des cœurs différents suppose un TSC synchronisé entre cœurs (vrai sur les x86 récents, non vérifié ici). Je n'ai pas vérifié la topologie (cœurs P / E, SMT) : choisir 4, 6, 8 suppose que des numéros pairs sont des cœurs physiques distincts.

Limites de cette partie : un seul instrument, un seul producteur et un seul consommateur par file (pas de MPMC), attente active (`cpu_pause()`, soit `_mm_pause` sur x86) donc 3 cœurs à 100 % pendant le run, pas de conflation, arrêt par un simple message de fin, message de taille fixe copié dans la file (24 et 32 octets). Les sanitizers (`-fsanitize=thread`, `address`, `undefined`) échouent à l'édition de liens avec le MinGW local (`cannot find -ltsan` / `-lubsan`, bibliothèques absentes de la distribution scoop) : ils tournent en CI Linux (jobs `asan-ubsan` et `threadsanitizer` de `.github/workflows/tests.yml`, ce dernier avec `-DATTENTE_COOPERATIVE` pour que l'attente reste praticable sous TSan). La vérification de concurrence repose donc sur le raisonnement de mémoire ci-dessus, sur les tests de séquence et sur TSan en CI ; x86-64 seulement, aucun job ARM.

![Détail SPSC contre mutex : débit, p50 par répétition, dispersion du p99](../figures/fig8_spsc_vs_mutex.png)

**Comment lire ce graphique.** Chaque point est une répétition, les traits noirs sont les médianes publiées. Les deux premiers panneaux (débit, p50) séparent nettement SPSC (bleu) et mutex (rouge) à chaque répétition. Le troisième panneau montre pourquoi on ne publie aucun écart sur le p99 : il varie de plusieurs ordres de grandeur d'une répétition à l'autre pour la **même** file, à cause de l'ordonnanceur de la machine.
