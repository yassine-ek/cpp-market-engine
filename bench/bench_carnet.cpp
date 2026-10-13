// Benchmark de latence par opération : (a) std::map vs (b) tableau plat.
//
// Méthode :
//  - Le flux (70 % ajouts, 25 % annulations, 5 % marché) est généré UNE fois, en pilotant un carnet
//    de référence : les annulations visent des ordres réellement vivants, les prix sont relatifs au
//    touch. Les deux implémentations rejouent exactement la même séquence.
//  - Latence par opération : compteur TSC (rdtsc) calibré sur steady_clock. steady_clock seul a ici une
//    granularité de 100 ns (QueryPerformanceCounter sous Windows), plus grosse que l'opération mesurée.
//  - Débit : passe séparée, sans chronométrage par opération, mesurée sur steady_clock.
#include <x86intrin.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "carnet.hpp"

using namespace marche;
using Horloge = std::chrono::steady_clock;

enum : uint8_t { AJOUT = 0, ANNUL = 1, MARCHE = 2 };
static const char* NOM_TYPE[3] = {"ajout", "annulation", "marche"};
struct Op { uint8_t type; uint8_t cote; Id id; Prix prix; Qte qte; };  // 16 octets

static size_t N_WARM = 500000, N_MESURE = 5000000;
static constexpr uint64_t GRAINE = 20260101;
// Avec 70 % d'ajouts et 25 % d'annulations, le carnet grossirait sans fin : la taille moyenne des ordres
// au marché est asservie au nombre d'ordres vivants (équilibre vers CIBLE_VIVANTS) pour rester stationnaire.
static constexpr double MOY_QTE_MARCHE = 320, CIBLE_VIVANTS = 50000;

// ------------------------------------------------------------------ horloge TSC
static inline uint64_t tsc_debut() { _mm_lfence(); const uint64_t t = __rdtsc(); _mm_lfence(); return t; }
static inline uint64_t tsc_fin() { unsigned aux; const uint64_t t = __rdtscp(&aux); _mm_lfence(); return t; }

static double calibrer_ticks_par_ns() {
  const auto t0 = Horloge::now();
  const uint64_t c0 = tsc_debut();
  while (Horloge::now() - t0 < std::chrono::milliseconds(300)) {}
  const uint64_t c1 = tsc_fin();
  const auto t1 = Horloge::now();
  return static_cast<double>(c1 - c0) / std::chrono::duration<double, std::nano>(t1 - t0).count();
}

static double resolution_steady_ns() {
  int64_t mn = INT64_MAX;
  for (int i = 0; i < 1000000; ++i) {
    const auto a = Horloge::now(), b = Horloge::now();
    const int64_t d = (b - a).count();
    if (d > 0 && d < mn) mn = d;
  }
  return static_cast<double>(mn);
}

// ------------------------------------------------------------------ génération du flux
struct StatsFlux { double vivants_moyen = 0; size_t vivants_max = 0, vivants_fin = 0, trades = 0, nb[3] = {0, 0, 0}; };

static std::vector<Op> generer_flux(size_t n, const Config& cfg, StatsFlux& st) {
  CarnetPlat ref(cfg);
  std::mt19937_64 rng(GRAINE);
  auto unif = [&] { return static_cast<double>(rng() >> 11) * 0x1.0p-53; };
  std::geometric_distribution<int> geo_offset(0.08);  // distance au touch, en ticks (moyenne ~12)
  std::vector<Op> ops;
  ops.reserve(n);
  std::vector<Id> vivants;
  std::vector<uint32_t> ou(n + 2, 0);  // position d'un id dans 'vivants'
  const Prix pmin = cfg.prix_ref - cfg.demi_largeur + 1, pmax = cfg.prix_ref + cfg.demi_largeur - 1;
  double somme_viv = 0;
  size_t nb_echant = 0;
  Id prochain = 1;

  auto oublier = [&](Id id) {
    const uint32_t p = ou[id];
    vivants[p] = vivants.back();
    ou[vivants[p]] = p;
    vivants.pop_back();
  };
  auto apres_trades = [&] {
    for (const Trade& t : ref.trades())
      if (!ref.contient(t.maker)) oublier(t.maker);
    st.trades += ref.trades().size();
    ref.vider_trades();
  };

  for (size_t k = 0; k < n; ++k) {
    const double u = unif();
    Op op{};
    op.cote = (rng() & 1) ? 1 : 0;
    const Cote c = static_cast<Cote>(op.cote);
    if (u < 0.70 || (u < 0.95 && vivants.empty())) {
      op.type = AJOUT;
      op.id = prochain++;
      const Prix bb = ref.meilleur(Cote::Achat), ba = ref.meilleur(Cote::Vente);
      const Prix ancre = c == Cote::Achat ? (ba ? ba : (bb ? bb + 2 : cfg.prix_ref))
                                          : (bb ? bb : (ba ? ba - 2 : cfg.prix_ref));
      const int signe = c == Cote::Achat ? -1 : 1;
      Prix p;
      if (unif() < 0.10) p = ancre - signe * static_cast<Prix>(rng() % 3);  // limite marchable
      else p = ancre + signe * (1 + geo_offset(rng));                        // passive
      op.prix = std::clamp(p, pmin, pmax);
      op.qte = 1 + static_cast<Qte>(rng() % 100);
      ref.ajouter_limite(op.id, c, op.prix, op.qte);
      apres_trades();
      if (ref.contient(op.id)) { ou[op.id] = static_cast<uint32_t>(vivants.size()); vivants.push_back(op.id); }
    } else if (u < 0.95) {
      op.type = ANNUL;
      op.id = vivants[rng() % vivants.size()];
      ref.annuler(op.id);
      oublier(op.id);
    } else {
      op.type = MARCHE;
      op.id = prochain++;
      const double moy = std::max(20.0, MOY_QTE_MARCHE * vivants.size() / CIBLE_VIVANTS);
      op.qte = 1 + static_cast<Qte>(std::log(1.0 - unif()) * -moy);  // exponentielle de moyenne ~moy
      ref.ordre_marche(op.id, c, op.qte);
      apres_trades();
    }
    ++st.nb[op.type];
    ops.push_back(op);
    if (k % 1000 == 0) {
      somme_viv += vivants.size();
      ++nb_echant;
      st.vivants_max = std::max(st.vivants_max, vivants.size());
    }
  }
  st.vivants_moyen = somme_viv / nb_echant;
  st.vivants_fin = vivants.size();
  return ops;
}

// ------------------------------------------------------------------ mesure
template <class C>
static inline void appliquer(C& c, const Op& op) {
  const Cote cote = static_cast<Cote>(op.cote);
  switch (op.type) {
    case AJOUT: c.ajouter_limite(op.id, cote, op.prix, op.qte); break;
    case ANNUL: c.annuler(op.id); break;
    default: c.ordre_marche(op.id, cote, op.qte); break;
  }
}

struct Perc { size_t n = 0; double p50 = 0, p99 = 0, p999 = 0, max = 0, moy = 0; };

static Perc percentiles(std::vector<uint32_t>& v, double ticks_par_ns) {
  Perc r;
  r.n = v.size();
  if (v.empty()) return r;
  std::sort(v.begin(), v.end());
  auto rang = [&](double p) { return v[std::min(v.size() - 1, static_cast<size_t>(std::ceil(p * v.size())) - 1)] / ticks_par_ns; };
  double s = 0;
  for (uint32_t x : v) s += x;
  r.p50 = rang(0.50); r.p99 = rang(0.99); r.p999 = rang(0.999);
  r.max = v.back() / ticks_par_ns;
  r.moy = s / v.size() / ticks_par_ns;
  return r;
}

struct Resultat {
  Perc global, type[3];
  double debit = 0;
  std::vector<double> debits, p50s;  // une valeur par répétition
  std::vector<double> bornes;
  std::vector<uint64_t> comptes;
  uint64_t nb_trades = 0, volume = 0;
};

template <class C>
static Resultat mesurer(const std::vector<Op>& ops, const Config& cfg, double tpn) {
  Resultat R;
  // 1) passe chronométrée opération par opération
  {
    C c(cfg);
    for (size_t i = 0; i < N_WARM; ++i) { appliquer(c, ops[i]); c.vider_trades(); }
    std::vector<uint32_t> lat(N_MESURE);
    for (size_t i = 0; i < N_MESURE; ++i) {
      const Op& op = ops[N_WARM + i];
      const uint64_t t0 = tsc_debut();
      appliquer(c, op);
      const uint64_t t1 = tsc_fin();
      lat[i] = static_cast<uint32_t>(std::min<uint64_t>(t1 - t0, UINT32_MAX));
      R.nb_trades += c.trades().size();
      for (const Trade& t : c.trades()) R.volume += t.qte;
      c.vider_trades();
    }
    // histogramme log : 10 classes par décade, de 1 ns à 10 ms
    const int nb = 70;
    R.bornes.resize(nb);
    R.comptes.assign(nb, 0);
    for (int k = 0; k < nb; ++k) R.bornes[k] = std::pow(10.0, k / 10.0);
    for (uint32_t x : lat) {
      const double ns = std::max(1.0, x / tpn);
      R.comptes[std::min(nb - 1, static_cast<int>(std::log10(ns) * 10.0))]++;
    }
    std::vector<uint32_t> par_type[3];
    for (size_t i = 0; i < N_MESURE; ++i) par_type[ops[N_WARM + i].type].push_back(lat[i]);
    for (int t = 0; t < 3; ++t) R.type[t] = percentiles(par_type[t], tpn);
    R.global = percentiles(lat, tpn);
  }
  // 2) débit : passe sans chronomètre par opération (le TSC coûte plusieurs ns, comparable à une opération)
  {
    C c(cfg);
    for (size_t i = 0; i < N_WARM; ++i) { appliquer(c, ops[i]); c.vider_trades(); }
    const auto t0 = Horloge::now();
    for (size_t i = 0; i < N_MESURE; ++i) { appliquer(c, ops[N_WARM + i]); c.vider_trades(); }
    const auto t1 = Horloge::now();
    R.debit = N_MESURE / std::chrono::duration<double>(t1 - t0).count();
  }
  return R;
}

static constexpr int REPS = 3;

// 3 répétitions alternées (a,b,a,b,...) : une dérive de la machine touche les deux de la même façon.
// On garde la répétition de p50 médian ; le débit publié est la médiane des débits.
static Resultat mediane(std::vector<Resultat>& v) {
  std::vector<double> debits, p50s;
  for (const Resultat& r : v) { debits.push_back(r.debit); p50s.push_back(r.global.p50); }
  std::vector<size_t> ordre(v.size());
  for (size_t i = 0; i < ordre.size(); ++i) ordre[i] = i;
  std::sort(ordre.begin(), ordre.end(), [&](size_t a, size_t b) { return p50s[a] < p50s[b]; });
  Resultat R = std::move(v[ordre[ordre.size() / 2]]);
  R.debits = debits;
  R.p50s = p50s;
  std::sort(debits.begin(), debits.end());
  R.debit = debits[debits.size() / 2];
  return R;
}

// ------------------------------------------------------------------ JSON
static void json_perc(FILE* f, const char* nom, const Perc& p, const char* fin) {
  std::fprintf(f, "      \"%s\": {\"n\": %zu, \"p50_ns\": %.1f, \"p99_ns\": %.1f, \"p999_ns\": %.1f, \"max_ns\": %.0f, \"moy_ns\": %.1f}%s\n",
               nom, p.n, p.p50, p.p99, p.p999, p.max, p.moy, fin);
}

static void json_resultat(FILE* f, const char* nom, const Resultat& R, const char* fin) {
  std::fprintf(f, "  \"%s\": {\n", nom);
  json_perc(f, "global", R.global, ",");
  for (int t = 0; t < 3; ++t) json_perc(f, NOM_TYPE[t], R.type[t], ",");
  std::fprintf(f, "      \"debit_ops_s\": %.0f,\n      \"debits_par_repetition\": [", R.debit);
  for (size_t k = 0; k < R.debits.size(); ++k) std::fprintf(f, "%s%.0f", k ? ", " : "", R.debits[k]);
  std::fprintf(f, "],\n      \"p50_ns_par_repetition\": [");
  for (size_t k = 0; k < R.p50s.size(); ++k) std::fprintf(f, "%s%.1f", k ? ", " : "", R.p50s[k]);
  std::fprintf(f, "],\n");
  std::fprintf(f, "      \"nb_trades\": %llu, \"volume\": %llu,\n", (unsigned long long)R.nb_trades, (unsigned long long)R.volume);
  std::fprintf(f, "      \"histogramme\": {\"bornes_basses_ns\": [");
  for (size_t k = 0; k < R.bornes.size(); ++k) std::fprintf(f, "%s%.2f", k ? ", " : "", R.bornes[k]);
  std::fprintf(f, "], \"comptes\": [");
  for (size_t k = 0; k < R.comptes.size(); ++k) std::fprintf(f, "%s%llu", k ? ", " : "", (unsigned long long)R.comptes[k]);
  std::fprintf(f, "]}\n  }%s\n", fin);
}

int main(int argc, char** argv) {
  int coeur = -2;  // -2 : coeur courant au lancement ; -1 : pas d'épinglage ; N >= 0 : coeur logique N (--coeur N)
  for (int i = 1; i + 1 < argc; ++i)
    if (!std::strcmp(argv[i], "--ops")) N_MESURE = std::stoull(argv[i + 1]);
    else if (!std::strcmp(argv[i], "--coeur")) coeur = std::stoi(argv[i + 1]);

#ifdef _WIN32
  // Coeur fixe + priorité haute : sinon les migrations de thread gonflent le p99.9 / max.
  SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
  if (coeur == -2) coeur = static_cast<int>(GetCurrentProcessorNumber());
  if (coeur >= 0) SetThreadAffinityMask(GetCurrentThread(), 1ULL << coeur);
#endif

  const Config cfg{10000, 2048, 1u << 20, static_cast<uint32_t>(N_WARM + N_MESURE + 16)};
  std::printf("calibration horloge...\n");
  const double tpn = calibrer_ticks_par_ns();
  uint64_t o[1001];
  for (int i = 0; i < 1001; ++i) { const uint64_t a = tsc_debut(); o[i] = tsc_fin() - a; }
  std::sort(o, o + 1001);
  const double overhead_ns = o[500] / tpn;
  const double res_ns = resolution_steady_ns();
  std::printf("TSC %.3f GHz, overhead mesure %.1f ns, granularite steady_clock %.0f ns\n", tpn, overhead_ns, res_ns);

  StatsFlux st;
  std::printf("generation du flux (%zu ops)...\n", N_WARM + N_MESURE);
  const std::vector<Op> ops = generer_flux(N_WARM + N_MESURE, cfg, st);
  std::printf("flux : %.0f ordres vivants en moyenne (max %zu, fin %zu), %zu trades\n", st.vivants_moyen, st.vivants_max,
              st.vivants_fin, st.trades);

  std::vector<Resultat> ra, rb;
  for (int rep = 0; rep < REPS; ++rep) {
    std::printf("repetition %d/%d : (a) map puis (b) plat...\n", rep + 1, REPS);
    ra.push_back(mesurer<CarnetMap>(ops, cfg, tpn));
    rb.push_back(mesurer<CarnetPlat>(ops, cfg, tpn));
  }
  const Resultat A = mediane(ra), B = mediane(rb);

  auto ligne = [](const char* n, const Resultat& R) {
    std::printf("%-12s p50 %7.1f ns  p99 %8.1f ns  p99.9 %9.1f ns  max %10.0f ns  debit %6.2f Mops/s\n", n, R.global.p50, R.global.p99,
                R.global.p999, R.global.max, R.debit / 1e6);
  };
  ligne("(a) map", A);
  ligne("(b) plat", B);
  if (A.nb_trades != B.nb_trades || A.volume != B.volume) std::printf("ATTENTION : les deux carnets divergent (trades/volume)\n");

  std::filesystem::create_directories("results");
  FILE* f = std::fopen("results/bench_carnet.json", "w");
  if (!f) { std::perror("results/bench_carnet.json"); return 1; }
  std::fprintf(f, "{\n  \"config\": {\"ops_mesurees\": %zu, \"ops_warmup\": %zu, \"mix\": {\"ajout\": 0.70, \"annulation\": 0.25, \"marche\": 0.05},\n"
                  "    \"graine\": %llu, \"prix_ref\": %d, \"demi_largeur_ticks\": %d, \"moy_qte_marche_a_cible\": %.0f, \"cible_vivants\": %.0f,\n"
                  "    \"ticks_par_ns\": %.4f, \"overhead_horloge_ns\": %.1f, \"granularite_steady_clock_ns\": %.0f,\n"
                  "    \"coeur_epingle\": %d, \"compilateur\": \"%s\", \"flags\": \"-O3 -march=native -std=c++20\"},\n",
               N_MESURE, N_WARM, (unsigned long long)GRAINE, cfg.prix_ref, cfg.demi_largeur, MOY_QTE_MARCHE, CIBLE_VIVANTS, tpn, overhead_ns, res_ns, coeur, __VERSION__);
  std::fprintf(f, "  \"flux\": {\"ordres_vivants_moyen\": %.0f, \"ordres_vivants_max\": %zu, \"ordres_vivants_fin\": %zu, \"trades_total\": %zu,\n"
                  "    \"nb_ajouts\": %zu, \"nb_annulations\": %zu, \"nb_marche\": %zu},\n",
               st.vivants_moyen, st.vivants_max, st.vivants_fin, st.trades, st.nb[0], st.nb[1], st.nb[2]);
  json_resultat(f, "carnet_map", A, ",");
  json_resultat(f, "carnet_plat", B, ",");
  std::fprintf(f, "  \"rapport_map_sur_plat\": {\"p50\": %.2f, \"p99\": %.2f, \"p999\": %.2f, \"debit_plat_sur_map\": %.2f}\n}\n",
               A.global.p50 / B.global.p50, A.global.p99 / B.global.p99, A.global.p999 / B.global.p999, B.debit / A.debit);
  std::fclose(f);
  std::printf("-> results/bench_carnet.json\n");
  return 0;
}
