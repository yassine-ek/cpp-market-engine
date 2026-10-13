# tests/ : tests sans framework

Chaque exécutable compte ses vérifications et ses échecs et renvoie un code de sortie non nul au moindre échec (donc directement utilisable en CI). Dernière sortie : `results/log_tests.txt`.

| Fichier | Vérifications | Contenu |
|---|---|---|
| `test_carnet.cpp` | 134 | Scénarios sur chaque carnet (FIFO, exécution partielle, annulation, croisement, balayage au marché, modification, rejets, bords de la fenêtre de prix) ; **test différentiel** : 1 000 000 puis 200 000 opérations aléatoires, comparaison à chaque pas du retour, de la liste exacte des trades, des meilleurs prix, des quantités et de la profondeur ; comptabilité des quantités et absence de croisement. |
| `test_spsc.cpp` | 162 | File SPSC : FIFO, vide, pleine, wrap-around, type composite ; deux threads sur 20 M d'éléments (0 perte, 0 doublon, 0 hors ordre) ; stress capacité 2 ; file mutex ; pipeline à 3 threads sur 2 M d'ordres (carnet final, nombre de trades et empreinte identiques à l'exécution mono-thread). |

Test de mutation (fait à la main, documenté dans le README de la racine) : casser volontairement le carnet plat fait échouer la suite (3 et 7 échecs) ; supprimer le test « file pleine » de `try_push` aussi (21 échecs).

Lancer : `bash build.sh` puis `./build/test_carnet` et `./build/test_spsc` (`.exe` sous MinGW).
