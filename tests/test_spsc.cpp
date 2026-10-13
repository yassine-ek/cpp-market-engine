// Tests de la file SPSC et du pipeline 3 threads. Même convention que test_carnet.cpp :
// pas de framework, un compteur de vérifications/échecs et un code de sortie.
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "carnet.hpp"
#include "file_mutex.hpp"
#include "pipeline.hpp"
#include "spsc.hpp"

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

// ----------------------------------------------------------------- mono-thread
static void test_fifo_vide_plein() {
  FileSpsc<int> q(8);
  int x = -1;
  VERIF(q.capacite() == 8);
  VERIF(!q.try_pop(x));  // vide au départ
  for (int i = 0; i < 8; ++i) VERIF(q.try_push(i));
  VERIF(!q.try_push(99));  // pleine : la 9e est refusée, rien n'est écrasé
  for (int i = 0; i < 8; ++i) {
    VERIF(q.try_pop(x));
    VERIF(x == i);  // FIFO
  }
  VERIF(!q.try_pop(x));  // de nouveau vide
  VERIF(q.try_push(42) && q.try_pop(x) && x == 42);
}

// Le wrap-around : on pousse/dépile par paquets de tailles variées pendant 1000 tours de la
// file, la séquence doit rester exacte alors que les indices dépassent largement la capacité.
static void test_wraparound() {
  FileSpsc<uint64_t> q(16);
  uint64_t ecrit = 0, lu = 0, attendu_ok = 0, anomalies = 0;  // anomalies : refus/accord à tort
  size_t dedans = 0;
  for (int tour = 0; tour < 16000; ++tour) {
    const size_t n_push = 1 + static_cast<size_t>(tour * 7 % 13);  // 1..13
    for (size_t k = 0; k < n_push; ++k) {
      const bool ok = q.try_push(ecrit);
      if (ok != (dedans < 16)) ++anomalies;
      if (ok) { ++ecrit; ++dedans; }
    }
    const size_t n_pop = 1 + static_cast<size_t>(tour * 5 % 11);  // 1..11
    for (size_t k = 0; k < n_pop; ++k) {
      uint64_t v = ~0ULL;
      const bool ok = q.try_pop(v);
      if (ok != (dedans > 0)) ++anomalies;
      if (ok) { if (v == lu) ++attendu_ok; ++lu; --dedans; }
    }
  }
  VERIF(anomalies == 0);        // plein/vide signalés exactement quand il faut
  VERIF(lu > 16 * 1000);        // plus de 1000 tours complets de la file
  VERIF(attendu_ok == lu);      // toutes les valeurs sorties dans l'ordre
}

struct Gros { uint64_t a, b, c; };  // type de 24 o comme MsgOrdre : la copie n'est pas atomique, la file ne doit pas la mélanger
static void test_type_composite() {
  FileSpsc<Gros> q(4);
  for (uint64_t i = 0; i < 100; ++i) {
    Gros g{i, i * 3, ~i}, o{};
    VERIF(q.try_push(g) && q.try_pop(o) && o.a == i && o.b == i * 3 && o.c == ~i);
  }
}

// ----------------------------------------------------------------- multi-thread
template <class File>
static void aller_retour(const char* nom, File& q, uint64_t n) {
  uint64_t manques = 0, attendu = 0, doublons_ou_pertes = 0, hors_ordre = 0;
  std::thread prod([&] {
    for (uint64_t i = 0; i < n; ++i)
      while (!q.try_push(i)) attendre();
  });
  std::thread cons([&] {
    uint64_t v;
    while (attendu < n) {
      if (!q.try_pop(v)) { attendre(); continue; }
      if (v != attendu) {
        if (v < attendu) ++doublons_ou_pertes; else ++manques;  // valeur déjà vue = doublon ; saut = perte
        ++hors_ordre;
        attendu = v;  // on se resynchronise pour ne compter qu'une fois par anomalie
      }
      ++attendu;
    }
  });
  prod.join();
  cons.join();
  uint64_t v;
  std::printf("  %s : %llu elements, pertes=%llu, doublons=%llu, hors ordre=%llu\n", nom, (unsigned long long)n,
              (unsigned long long)manques, (unsigned long long)doublons_ou_pertes, (unsigned long long)hors_ordre);
  VERIF(manques == 0);
  VERIF(doublons_ou_pertes == 0);
  VERIF(hors_ordre == 0);
  VERIF(attendu == n);
  VERIF(!q.try_pop(v));  // rien en trop après le dernier élément
}

static void test_multithread() {
  { FileSpsc<uint64_t> q(1024); aller_retour("SPSC capacite 1024 ", q, 20000000); }
  { FileSpsc<uint64_t> q(2); aller_retour("SPSC capacite 2    ", q, 3000000); }  // file toujours pleine/vide : stress des deux bords
  { FileMutex<uint64_t> q(1024); aller_retour("Mutex capacite 1024", q, 2000000); }
}

// ----------------------------------------------------------------- pipeline
static bool memes_carnets(const CarnetPlat& a, const CarnetPlat& b) {
  if (a.nb_ordres() != b.nb_ordres()) return false;
  for (Cote c : {Cote::Achat, Cote::Vente}) {
    if (a.meilleur(c) != b.meilleur(c) || a.qte_totale(c) != b.qte_totale(c)) return false;
    NiveauVue va[40], vb[40];
    const int na = a.profondeur(c, va, 40), nb = b.profondeur(c, vb, 40);
    if (na != nb) return false;
    for (int i = 0; i < na; ++i)
      if (va[i].prix != vb[i].prix || va[i].qte != vb[i].qte) return false;
  }
  return true;
}

template <template <class> class File>
static void test_pipeline(const char* nom, const std::vector<MsgOrdre>& ops, const Config& cfg, const ResultatPipeline& ref,
                          size_t capacite) {
  OptPipeline opt;
  opt.capacite = capacite;
  ResultatPipeline r = executer_pipeline<File>(ops, cfg, opt);
  const bool carnet_ok = memes_carnets(*r.carnet, *ref.carnet);
  std::printf("  %s (cap %zu) : %llu msgs vus, trades=%llu (ref %llu), carnet identique=%d, empreinte identique=%d\n", nom,
              capacite, (unsigned long long)r.strat.nb_msgs, (unsigned long long)r.nb_trades,
              (unsigned long long)ref.nb_trades, carnet_ok, r.strat == ref.strat);
  VERIF(r.strat.nb_msgs == ops.size());  // un top par ordre, aucun perdu
  VERIF(carnet_ok);                       // carnet final identique au mono-thread
  VERIF(r.nb_trades == ref.nb_trades);
  VERIF(r.strat == ref.strat);            // la stratégie a vu exactement la même suite de tops (empreinte sensible à l'ordre)
}

static void test_pipelines() {
  const size_t N = 2000000;
  const Config cfg{10000, 2048, 1u << 20, static_cast<uint32_t>(N + 16)};
  const std::vector<MsgOrdre> ops = generer_ordres(N, 20261013, cfg);
  const ResultatPipeline ref = reference_mono(ops, cfg);
  std::printf("  reference mono-thread : %zu msgs, %llu trades, %zu ordres au repos, %llu hausses, %llu baisses, %llu spreads serres\n",
              N, (unsigned long long)ref.nb_trades, ref.carnet->nb_ordres(), (unsigned long long)ref.strat.nb_hausses,
              (unsigned long long)ref.strat.nb_baisses, (unsigned long long)ref.strat.nb_spread_serre);
  VERIF(ref.nb_trades > N / 20);                       // le flux exerce l'appariement
  VERIF(ref.strat.nb_hausses > 1000 && ref.strat.nb_baisses > 1000);  // et la stratégie voit bouger le mid
  test_pipeline<FileSpsc>("pipeline SPSC ", ops, cfg, ref, 1024);
  test_pipeline<FileSpsc>("pipeline SPSC ", ops, cfg, ref, 4);     // forte contre-pression entre étages
  test_pipeline<FileMutex>("pipeline Mutex", ops, cfg, ref, 1024);
  // un autre flux : la référence doit changer (sinon l'empreinte ne prouverait rien)
  const std::vector<MsgOrdre> ops2 = generer_ordres(N, 777, cfg);
  VERIF(reference_mono(ops2, cfg).strat.empreinte != ref.strat.empreinte);
}

int main() {
  std::printf("[file SPSC, mono-thread]\n");
  test_fifo_vide_plein();
  test_wraparound();
  test_type_composite();
  std::printf("[file SPSC, 2 threads]\n");
  test_multithread();
  std::printf("[pipeline 3 threads]\n");
  test_pipelines();
  std::printf("\n%d verifications, %d echec(s)\n", verifs, echecs);
  return echecs == 0 ? 0 : 1;
}
