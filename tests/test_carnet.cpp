// Tests des deux carnets. Pas de framework : un compteur d'échecs et un code de sortie.
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include "carnet.hpp"

using namespace marche;

static int echecs = 0;
static int verifs = 0;
#define VERIF(cond)                                                          \
  do {                                                                       \
    ++verifs;                                                                \
    if (!(cond)) {                                                           \
      ++echecs;                                                              \
      std::printf("  ECHEC %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
    }                                                                        \
  } while (0)

static Config cfg_test() { return Config{10000, 2048, 1 << 16, 1 << 16}; }

// Extrait les trades courants puis vide le journal.
template <class C>
static std::vector<Trade> prendre(C& c) {
  std::vector<Trade> t = c.trades();
  c.vider_trades();
  return t;
}

template <class C>
static void test_fifo() {
  C c(cfg_test());
  for (Id i = 1; i <= 3; ++i) c.ajouter_limite(i, Cote::Vente, 10100, 10);
  VERIF(c.ordre_marche(9, Cote::Achat, 25) == 25);
  auto t = prendre(c);
  VERIF(t.size() == 3);
  VERIF(t[0].maker == 1 && t[0].qte == 10 && t[0].prix == 10100);
  VERIF(t[1].maker == 2 && t[1].qte == 10);
  VERIF(t[2].maker == 3 && t[2].qte == 5);
  VERIF(c.qte_ordre(3) == 5 && !c.contient(1) && !c.contient(2));
  VERIF(c.qte_niveau(Cote::Vente, 10100) == 5 && c.nb_ordres() == 1);
}

template <class C>
static void test_execution_partielle() {
  C c(cfg_test());
  c.ajouter_limite(1, Cote::Vente, 10100, 10);
  c.ajouter_limite(2, Cote::Vente, 10100, 10);
  c.ajouter_limite(3, Cote::Achat, 10100, 4);  // croise : prend 4 sur l'ordre 1, rien ne repose
  auto t = prendre(c);
  VERIF(t.size() == 1 && t[0].maker == 1 && t[0].taker == 3 && t[0].qte == 4);
  VERIF(!c.contient(3) && c.qte_ordre(1) == 6);
  // l'ordre 1 partiellement exécuté garde sa priorité sur le 2
  c.ordre_marche(4, Cote::Achat, 8);
  t = prendre(c);
  VERIF(t.size() == 2 && t[0].maker == 1 && t[0].qte == 6 && t[1].maker == 2 && t[1].qte == 2);
}

template <class C>
static void test_annulation() {
  C c(cfg_test());
  c.ajouter_limite(1, Cote::Achat, 9990, 10);
  c.ajouter_limite(2, Cote::Achat, 9990, 5);
  VERIF(c.annuler(1));
  VERIF(!c.annuler(1));    // déjà annulé
  VERIF(!c.annuler(777));  // inconnu
  VERIF(c.qte_niveau(Cote::Achat, 9990) == 5 && c.meilleur(Cote::Achat) == 9990);
  VERIF(c.annuler(2));
  VERIF(c.meilleur(Cote::Achat) == SANS_PRIX && c.nb_ordres() == 0 && c.qte_totale(Cote::Achat) == 0);
  VERIF(c.ordre_marche(3, Cote::Vente, 5) == 0);  // plus rien à frapper
}

template <class C>
static void test_croisement() {
  C c(cfg_test());
  c.ajouter_limite(10, Cote::Vente, 10101, 5);
  c.ajouter_limite(11, Cote::Vente, 10102, 5);
  c.ajouter_limite(12, Cote::Vente, 10103, 5);
  c.ajouter_limite(1, Cote::Achat, 9999, 5);
  VERIF(c.ajouter_limite(20, Cote::Achat, 10102, 12));  // balaie 10101 et 10102, reste 2 à 10102
  auto t = prendre(c);
  VERIF(t.size() == 2);
  VERIF(t[0].maker == 10 && t[0].prix == 10101 && t[0].qte == 5);
  VERIF(t[1].maker == 11 && t[1].prix == 10102 && t[1].qte == 5);  // prix du maker, pas du taker
  VERIF(c.meilleur(Cote::Achat) == 10102 && c.qte_ordre(20) == 2);
  VERIF(c.meilleur(Cote::Vente) == 10103);
  VERIF(c.meilleur(Cote::Achat) < c.meilleur(Cote::Vente));  // jamais croisé
  VERIF(c.qte_totale(Cote::Achat) == 7 && c.qte_totale(Cote::Vente) == 5);
}

template <class C>
static void test_marche_balaye() {
  C c(cfg_test());
  for (int k = 0; k < 3; ++k) c.ajouter_limite(10 + k, Cote::Vente, 10101 + k, 10);
  c.ajouter_limite(1, Cote::Achat, 9999, 7);
  VERIF(c.ordre_marche(50, Cote::Achat, 25) == 25);
  auto t = prendre(c);
  VERIF(t.size() == 3 && t[2].prix == 10103 && t[2].qte == 5);
  VERIF(c.qte_niveau(Cote::Vente, 10103) == 5 && c.meilleur(Cote::Vente) == 10103);
  // ordre plus gros que le carnet : le reliquat est abandonné, il ne repose pas
  VERIF(c.ordre_marche(51, Cote::Achat, 100) == 5);
  VERIF(c.meilleur(Cote::Vente) == SANS_PRIX && !c.contient(51));
  VERIF(c.qte_totale(Cote::Achat) == 7);  // le côté achat n'a pas bougé
  VERIF(c.ordre_marche(52, Cote::Achat, 10) == 0);
}

template <class C>
static void test_modification() {
  C c(cfg_test());
  c.ajouter_limite(1, Cote::Vente, 10100, 10);
  c.ajouter_limite(2, Cote::Vente, 10100, 10);
  VERIF(c.modifier(1, 10100, 5));  // baisse : garde la priorité
  c.ordre_marche(9, Cote::Achat, 7);
  auto t = prendre(c);
  VERIF(t.size() == 2 && t[0].maker == 1 && t[0].qte == 5 && t[1].maker == 2 && t[1].qte == 2);

  C d(cfg_test());
  d.ajouter_limite(1, Cote::Vente, 10100, 10);
  d.ajouter_limite(2, Cote::Vente, 10100, 10);
  VERIF(d.modifier(1, 10100, 20));  // hausse : passe derrière le 2
  d.ordre_marche(9, Cote::Achat, 5);
  t = prendre(d);
  VERIF(t.size() == 1 && t[0].maker == 2 && t[0].qte == 5);
  VERIF(d.qte_niveau(Cote::Vente, 10100) == 25 && d.qte_totale(Cote::Vente) == 25);

  C e(cfg_test());
  e.ajouter_limite(5, Cote::Achat, 10000, 10);
  e.ajouter_limite(6, Cote::Vente, 10002, 10);
  VERIF(e.modifier(5, 10002, 10));  // changement de prix qui croise : exécution immédiate
  t = prendre(e);
  VERIF(t.size() == 1 && t[0].maker == 6 && t[0].taker == 5 && t[0].prix == 10002);
  VERIF(e.nb_ordres() == 0);
  VERIF(!e.modifier(5, 10002, 10));  // l'ordre n'existe plus

  C f(cfg_test());
  f.ajouter_limite(1, Cote::Achat, 10000, 10);
  VERIF(!f.modifier(1, 99999, 10));  // hors fenêtre : refusé, l'ordre reste intact
  VERIF(f.contient(1) && f.meilleur(Cote::Achat) == 10000);
  VERIF(!f.modifier(1, 10000, 0));
}

template <class C>
static void test_rejets() {
  C c(cfg_test());
  VERIF(c.ajouter_limite(1, Cote::Achat, 10000, 10));
  VERIF(!c.ajouter_limite(1, Cote::Vente, 10005, 10));  // id déjà vivant
  VERIF(!c.ajouter_limite(2, Cote::Achat, 10000, 0));
  VERIF(!c.ajouter_limite(2, Cote::Achat, 10000, -3));
  VERIF(!c.ajouter_limite(2, Cote::Achat, 1, 5));        // hors fenêtre
  VERIF(!c.ajouter_limite(2, Cote::Achat, 20000, 5));
  VERIF(c.nb_ordres() == 1);
  c.vider();
  VERIF(c.nb_ordres() == 0 && c.ajouter_limite(1, Cote::Achat, 10000, 10));  // id réutilisable après vider
}

// Bords exacts de la fenêtre [prix_ref - demi, prix_ref + demi] : le plat indexe un tableau par prix - base,
// donc un bord mal borné serait un accès hors tableau. Le différentiel ne le voit pas (les deux carnets partagent valide()).
template <class C>
static void test_bords_fenetre() {
  C c(cfg_test());
  const Prix bas = 10000 - 2048, haut = 10000 + 2048;
  VERIF(c.ajouter_limite(1, Cote::Achat, bas, 5));        // bord bas accepté
  VERIF(c.ajouter_limite(2, Cote::Vente, haut, 7));       // bord haut accepté
  VERIF(!c.ajouter_limite(3, Cote::Achat, bas - 1, 5));   // un tick en dehors : refusé
  VERIF(!c.ajouter_limite(4, Cote::Vente, haut + 1, 5));
  VERIF(c.meilleur(Cote::Achat) == bas && c.meilleur(Cote::Vente) == haut);
  VERIF(c.qte_niveau(Cote::Achat, bas) == 5 && c.qte_niveau(Cote::Vente, haut) == 7);
  VERIF(c.qte_niveau(Cote::Achat, bas - 1) == 0 && c.qte_niveau(Cote::Vente, haut + 1) == 0);  // pas d'accès hors tableau
  VERIF(!c.modifier(1, bas - 1, 5) && c.contient(1));     // modification hors fenêtre : refusée, l'ordre reste
  NiveauVue v[2];
  VERIF(c.profondeur(Cote::Achat, v, 2) == 1 && v[0].prix == bas && v[0].qte == 5);
  VERIF(c.ordre_marche(9, Cote::Achat, 100) == 7 && c.meilleur(Cote::Vente) == SANS_PRIX);  // vide le bord haut
  VERIF(c.ordre_marche(10, Cote::Vente, 100) == 5 && c.meilleur(Cote::Achat) == SANS_PRIX);  // vide le bord bas
  VERIF(c.nb_ordres() == 0);
}

// ---------- flux aléatoire : équivalence des deux carnets + comptabilité des quantités ----------
static int64_t total(const CarnetMap& c) { return c.qte_totale(Cote::Achat) + c.qte_totale(Cote::Vente); }

static int64_t qte_trades(const std::vector<Trade>& t, Id taker) {
  int64_t s = 0;
  for (const Trade& x : t) if (x.taker == taker) s += x.qte;
  return s;
}

static void test_differentiel(size_t n_ops, uint64_t graine) {
  CarnetMap a(Config{10000, 2048, 1 << 16, 1 << 16});
  CarnetPlat b(Config{10000, 2048, 1 << 16, 1 << 16});
  std::mt19937_64 rng(graine);
  auto U = [&](int m) { return static_cast<int>(rng() % static_cast<uint64_t>(m)); };
  // Comptabilité indépendante des carnets : quantité totale au repos attendue (achats + ventes).
  // Un ordre limite de q dont f sont exécutés en taker retire f au côté opposé et pose q-f : delta = q - 2f.
  int64_t attendu = 0;
  size_t nb_trades = 0, desaccords = 0, incoherences = 0;
  Id prochain = 1;

  for (size_t k = 0; k < n_ops; ++k) {
    const int type = U(100);
    const Cote c = U(2) ? Cote::Achat : Cote::Vente;
    const Prix bb = a.meilleur(Cote::Achat), ba = a.meilleur(Cote::Vente);
    const Prix mid = (bb && ba) ? (bb + ba) / 2 : (bb ? bb : (ba ? ba : 10000));
    const Qte q = 1 + U(50);
    bool ra = false, rb = false;
    Qte ea = 0, eb = 0;

    if (type < 56) {  // limite : passive, agressive (traverse des niveaux), ou prix absurde (rejeté)
      const Id id = prochain++;
      Prix p;
      Qte qq = q;
      if (type < 45) p = c == Cote::Achat ? mid - U(15) : mid + U(15);
      else if (type < 55) { p = c == Cote::Achat ? mid + U(6) : mid - U(6); qq = 3 * q; }
      else p = U(2) ? 5 : 99999;
      ra = a.ajouter_limite(id, c, p, qq);
      rb = b.ajouter_limite(id, c, p, qq);
      if (ra) attendu += qq - 2 * qte_trades(a.trades(), id);
    } else if (type < 75) {  // annulation, parfois d'un ordre déjà mort
      const Id id = prochain > 3000 ? prochain - 1 - U(3000) : 1 + U(static_cast<int>(prochain));
      const Qte avant = a.qte_ordre(id);
      ra = a.annuler(id);
      rb = b.annuler(id);
      if (ra) attendu -= avant;
    } else if (type < 85) {  // ordre au marché
      const Id id = prochain++;
      ea = a.ordre_marche(id, c, 2 * q);
      eb = b.ordre_marche(id, c, 2 * q);
      attendu -= ea;
      ra = rb = true;
    } else {  // modification : baisse, hausse, ou changement de prix (peut croiser)
      const Id id = prochain > 3000 ? prochain - 1 - U(3000) : 1 + U(static_cast<int>(prochain));
      const Qte avant = a.qte_ordre(id);
      const Prix p = U(2) ? mid - 5 + U(10) : mid;
      ra = a.modifier(id, p, q);
      rb = b.modifier(id, p, q);
      if (ra) attendu += q - avant - 2 * qte_trades(a.trades(), id);
    }

    // 1) mêmes retours, mêmes trades, dans le même ordre
    if (ra != rb || ea != eb || a.trades() != b.trades()) ++desaccords;
    nb_trades += a.trades().size();
    a.vider_trades();
    b.vider_trades();

    // 2) même état visible, quantités cohérentes avec la comptabilité, carnet jamais croisé
    if (a.meilleur(Cote::Achat) != b.meilleur(Cote::Achat) || a.meilleur(Cote::Vente) != b.meilleur(Cote::Vente) ||
        a.qte_totale(Cote::Achat) != b.qte_totale(Cote::Achat) ||
        a.qte_totale(Cote::Vente) != b.qte_totale(Cote::Vente) || a.nb_ordres() != b.nb_ordres())
      ++desaccords;
    if (total(a) != attendu) ++incoherences;
    const Prix xb = a.meilleur(Cote::Achat), xa = a.meilleur(Cote::Vente);
    if (xb && xa && xb >= xa) ++incoherences;

    if (k % 2000 == 0) {  // profondeur sur 8 niveaux, niveau par niveau
      NiveauVue va[8], vb[8];
      for (Cote s : {Cote::Achat, Cote::Vente}) {
        const int na = a.profondeur(s, va, 8), nb = b.profondeur(s, vb, 8);
        if (na != nb) ++desaccords;
        for (int i = 0; i < na && i < nb; ++i)
          if (va[i].prix != vb[i].prix || va[i].qte != vb[i].qte) ++desaccords;
      }
    }
  }
  std::printf("  differentiel : %zu ops, %zu trades, ordres au repos a la fin=%zu, desaccords=%zu, incoherences=%zu\n",
              n_ops, nb_trades, a.nb_ordres(), desaccords, incoherences);
  VERIF(desaccords == 0);
  VERIF(incoherences == 0);
  VERIF(nb_trades > n_ops / 20);  // le flux exerce vraiment l'appariement
}

template <class C>
static void suite(const char* nom) {
  std::printf("[%s]\n", nom);
  test_fifo<C>();
  test_execution_partielle<C>();
  test_annulation<C>();
  test_croisement<C>();
  test_marche_balaye<C>();
  test_modification<C>();
  test_rejets<C>();
  test_bords_fenetre<C>();
}

int main() {
  suite<CarnetMap>("CarnetMap (a)");
  suite<CarnetPlat>("CarnetPlat (b)");
  std::printf("[flux aleatoire]\n");
  test_differentiel(1000000, 12345);
  test_differentiel(200000, 987654321);
  std::printf("\n%d verifications, %d echec(s)\n", verifs, echecs);
  return echecs == 0 ? 0 : 1;
}
