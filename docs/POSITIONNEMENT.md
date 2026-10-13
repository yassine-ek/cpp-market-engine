# Positionnement face à d'autres dépôts GitHub

Source : lecture des README et des métadonnées publiques de ces dépôts en octobre 2026. **Je n'ai ni lu ni exécuté leur code** : je ne compare que ce que leurs README annoncent. Le but n'est pas de rivaliser avec ces outils.

| Dépôt | Ce que c'est (d'après le README) | Différence avec ce dépôt |
|---|---|---|
| [enewhuis/liquibook](https://github.com/enewhuis/liquibook) | moteur d'appariement C++ avec listeners de fills | périmètre plus large ; ici, comparaison mesurée de deux structures de données et test différentiel |
| [Kautenja/limit-order-book](https://github.com/Kautenja/limit-order-book) | carnet C++ avec binding Python et benchmarks | même axe « carnet C++ benchmarké » ; ici deux implémentations face à face sur le même flux, latence par opération séparée du débit |
| [Leotaby/MicroExchange](https://github.com/Leotaby/MicroExchange) | moteur CLOB C++20, replay ITCH, analytique de microstructure | le plus proche par le langage ; couvre le replay de données réelles, ce que ce dépôt ne fait pas ; ici market making Avellaneda-Stoikov et période de recotation |
| [tspooner/rl_markets](https://github.com/tspooner/rl_markets) | code C++ de « Market making via reinforcement learning » : simulateur de LOB, TD learning | ici un market maker analytique (AS) comparé à un naïf, Monte Carlo de sessions reproductible |
| [nkaz001/hftbacktest](https://github.com/nkaz001/hftbacktest) | backtester rejouant un carnet L2/L3 avec latence de flux, latence d'ordre et position dans la file | niveau de réalisme que ce dépôt n'a pas : ni file d'attente réseau ni données réelles ; la « latence » est ici une période de recotation en temps simulé |
| [abides-sim/abides](https://github.com/abides-sim/abides) | simulation multi-agents avec latences réseau configurables | même idée (agents ZI, market maker, latence), beaucoup plus complet et plus lourd ; ici un fichier C++ qui simule 25 000 sessions en 12 s |

Ce que ce dépôt montre : la chaîne complète en peu de code et vérifiée (carnet testé en différentiel, benchmark de latence soigné sur ses biais d'horloge, simulation de market making avec calibration et deux régimes de flux, pricer validé par une forme fermée), avec des limites écrites.
