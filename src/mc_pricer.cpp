// Pricer Monte Carlo multithread : call européen et call asiatique arithmétique sous GBM.
//  - un mt19937_64 PAR THREAD, graines distinctes dérivées par SplitMix64 ;
//  - variables antithétiques : chaque tirage Z sert pour (Z, -Z) et l'on traite la MOYENNE du couple comme
//    une observation i.i.d. (l'erreur-type est calculée sur ces couples, pas sur les 2n trajectoires) ;
//  - chaque thread accumule en local puis écrit une fois : pas de verrou, pas de faux partage (alignas 64).
// Validations : européen vs Black-Scholes ; asiatique GÉOMÉTRIQUE (calculé sur les mêmes trajectoires) vs formule
// fermée, qui valide le schéma de discrétisation des trajectoires dont dépend l'asiatique arithmétique.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <filesystem>
#include <random>
#include <thread>
#include <vector>

#include "util.hpp"

struct Params {
  double S0 = 100, K = 100, T = 1, r = 0.02, sigma = 0.2;
  int M = 50;  // dates de fixing de l'asiatique, équiréparties dans ]0, T]
};

struct alignas(64) Partiel {
  double e = 0, e2 = 0;   // européen : somme et somme des carrés des payoffs de couples
  double a = 0, a2 = 0;   // asiatique arithmétique
  double g = 0, g2 = 0;   // asiatique géométrique (validation)
  uint64_t n = 0;
};

static double phi(double x) { return 0.5 * std::erfc(-x / std::sqrt(2.0)); }

static double black_scholes_call(const Params& p) {
  const double d1 = (std::log(p.S0 / p.K) + (p.r + 0.5 * p.sigma * p.sigma) * p.T) / (p.sigma * std::sqrt(p.T));
  const double d2 = d1 - p.sigma * std::sqrt(p.T);
  return p.S0 * phi(d1) - p.K * std::exp(-p.r * p.T) * phi(d2);
}

// Asiatique géométrique à fixings discrets : ln G est gaussien (moyenne et variance en forme fermée).
static double asiatique_geometrique_exact(const Params& p) {
  const double M = p.M;
  const double mu = std::log(p.S0) + (p.r - 0.5 * p.sigma * p.sigma) * p.T * (M + 1) / (2 * M);
  const double v = p.sigma * p.sigma * p.T * (M + 1) * (2 * M + 1) / (6 * M * M);
  const double d1 = (mu - std::log(p.K) + v) / std::sqrt(v);
  const double d2 = d1 - std::sqrt(v);
  return std::exp(-p.r * p.T) * (std::exp(mu + 0.5 * v) * phi(d1) - p.K * phi(d2));
}

static void travail_europeen(const Params& p, uint64_t paires, uint64_t graine, Partiel& out) {
  std::mt19937_64 gen(graine);
  std::normal_distribution<double> N01;
  const double drift = (p.r - 0.5 * p.sigma * p.sigma) * p.T, vol = p.sigma * std::sqrt(p.T);
  Partiel l;
  for (uint64_t i = 0; i < paires; ++i) {
    const double z = N01(gen);
    const double y = 0.5 * (std::max(p.S0 * std::exp(drift + vol * z) - p.K, 0.0) + std::max(p.S0 * std::exp(drift - vol * z) - p.K, 0.0));
    l.e += y;
    l.e2 += y * y;
  }
  l.n = paires;
  out = l;
}

static void travail_asiatique(const Params& p, uint64_t paires, uint64_t graine, Partiel& out) {
  std::mt19937_64 gen(graine);
  std::normal_distribution<double> N01;
  const double dt = p.T / p.M, drift = (p.r - 0.5 * p.sigma * p.sigma) * dt, vol = p.sigma * std::sqrt(dt);
  Partiel l;
  for (uint64_t i = 0; i < paires; ++i) {
    double xp = 0, xm = 0;      // ln(S_t / S0) le long de la trajectoire +Z et de son antithétique -Z
    double sp = 0, sm = 0;      // sommes de S_t / S0 (moyenne arithmétique)
    double lp = 0, lm = 0;      // sommes de ln(S_t / S0) (moyenne géométrique)
    for (int j = 0; j < p.M; ++j) {
      const double z = N01(gen);
      xp += drift + vol * z;
      xm += drift - vol * z;
      sp += std::exp(xp); sm += std::exp(xm);
      lp += xp; lm += xm;
    }
    const double a = 0.5 * (std::max(p.S0 * sp / p.M - p.K, 0.0) + std::max(p.S0 * sm / p.M - p.K, 0.0));
    const double g = 0.5 * (std::max(p.S0 * std::exp(lp / p.M) - p.K, 0.0) + std::max(p.S0 * std::exp(lm / p.M) - p.K, 0.0));
    l.a += a; l.a2 += a * a;
    l.g += g; l.g2 += g * g;
  }
  l.n = paires;
  out = l;
}

struct Estimation { double prix, se; };

static Estimation estimer(double somme, double somme2, uint64_t n, double actu) {
  const double m = somme / n, var = std::max(0.0, somme2 / n - m * m) * n / (n - 1);
  return {actu * m, actu * std::sqrt(var / n)};
}

using Travail = void (*)(const Params&, uint64_t, uint64_t, Partiel&);

// Lance 'paires' couples répartis sur nt threads ; renvoie la somme des accumulateurs et la durée (s).
static double lancer(Travail f, const Params& p, uint64_t paires, int nt, uint64_t graine, Partiel& total) {
  std::vector<Partiel> parts(nt);
  std::vector<std::thread> th;
  const auto t0 = std::chrono::steady_clock::now();
  for (int t = 0; t < nt; ++t) {
    const uint64_t n_t = paires / nt + (static_cast<uint64_t>(t) < paires % nt ? 1 : 0);
    th.emplace_back(f, std::cref(p), n_t, splitmix64(graine + t), std::ref(parts[t]));
  }
  for (auto& x : th) x.join();
  const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  total = Partiel{};
  for (const Partiel& q : parts) {
    total.e += q.e; total.e2 += q.e2; total.a += q.a; total.a2 += q.a2; total.g += q.g; total.g2 += q.g2; total.n += q.n;
  }
  return dt;
}

int main(int argc, char** argv) {
  Params p;
  uint64_t paires_euro = 20000000, paires_asia = 1000000, graine = 20260101;
  int reps = 5;
  for (int i = 1; i + 1 < argc; ++i) {
    if (!std::strcmp(argv[i], "--paires-euro")) paires_euro = std::stoull(argv[i + 1]);
    else if (!std::strcmp(argv[i], "--paires-asia")) paires_asia = std::stoull(argv[i + 1]);
    else if (!std::strcmp(argv[i], "--reps")) reps = std::stoi(argv[i + 1]);
    else if (!std::strcmp(argv[i], "--graine")) graine = std::stoull(argv[i + 1]);
  }
  const double actu = std::exp(-p.r * p.T);
  const double bs = black_scholes_call(p), geo_exact = asiatique_geometrique_exact(p);
  const unsigned hw = std::thread::hardware_concurrency();
  std::printf("GBM S0=%.0f K=%.0f T=%.0f r=%.2f sigma=%.2f ; asiatique : %d fixings ; %u threads materiels\n", p.S0, p.K, p.T, p.r, p.sigma, p.M, hw);
  std::printf("%llu couples antithetiques (europeen), %llu (asiatique)\n\n", (unsigned long long)paires_euro, (unsigned long long)paires_asia);

  {  // chauffe : la première mesure à 1 thread paierait la montée en fréquence du CPU et les défauts de page
    Partiel w;
    lancer(travail_europeen, p, paires_euro / 4, 1, graine, w);
    lancer(travail_asiatique, p, paires_asia / 4, 1, graine, w);
  }
  const std::vector<int> nts = {1, 2, 4, 8, 16};
  struct Ligne { int nt; double t_e, t_a; };
  std::vector<Ligne> lignes;
  Partiel pe, pa;  // résultats conservés du dernier nombre de threads (le plus grand)
  for (int nt : nts) {
    double te = 1e30, ta = 1e30;
    for (int r = 0; r < reps; ++r) {  // meilleur de 'reps' : on mesure le calcul, pas le bruit de l'OS
      Partiel x, y;
      te = std::min(te, lancer(travail_europeen, p, paires_euro, nt, graine, x));
      ta = std::min(ta, lancer(travail_asiatique, p, paires_asia, nt, graine + 1000, y));
      pe = x; pa = y;
    }
    lignes.push_back({nt, te, ta});
    std::printf("threads=%2d  europeen %.3f s (x%.2f)   asiatique %.3f s (x%.2f)\n", nt, te, lignes[0].t_e / te, ta, lignes[0].t_a / ta);
  }

  const Estimation E = estimer(pe.e, pe.e2, pe.n, actu), A = estimer(pa.a, pa.a2, pa.n, actu), G = estimer(pa.g, pa.g2, pa.n, actu);
  const double zE = (E.prix - bs) / E.se, zG = (G.prix - geo_exact) / G.se;
  std::printf("\nCall europeen   : %.5f  erreur-type %.5f  IC95 [%.5f ; %.5f]\n", E.prix, E.se, E.prix - 1.96 * E.se, E.prix + 1.96 * E.se);
  std::printf("Black-Scholes   : %.5f  ecart %+.5f = %+.2f erreurs-types  -> %s\n", bs, E.prix - bs, zE, std::fabs(zE) < 3 ? "OK (< 3 ET)" : "ECHEC (>= 3 ET)");
  std::printf("Call asiatique  : %.5f  erreur-type %.5f  IC95 [%.5f ; %.5f]\n", A.prix, A.se, A.prix - 1.96 * A.se, A.prix + 1.96 * A.se);
  std::printf("  geometrique   : MC %.5f (ET %.5f)  forme fermee %.5f  ecart %+.2f ET -> %s\n", G.prix, G.se, geo_exact, zG,
              std::fabs(zG) < 3 ? "OK" : "ECHEC");
  std::printf("  coherence     : geometrique <= arithmetique <= europeen : %s\n",
              (G.prix <= A.prix && A.prix <= E.prix) ? "OK" : "VIOLEE");

  std::filesystem::create_directories("results");
  FILE* f = std::fopen("results/mc_pricer.json", "w");
  if (!f) { std::perror("results/mc_pricer.json"); return 1; }
  std::fprintf(f, "{\n  \"parametres\": {\"S0\": %.0f, \"K\": %.0f, \"T\": %.0f, \"r\": %.2f, \"sigma\": %.2f, \"fixings_asiatique\": %d,\n"
                  "    \"couples_antithetiques_europeen\": %llu, \"couples_antithetiques_asiatique\": %llu, \"graine\": %llu, \"threads_materiels\": %u},\n",
               p.S0, p.K, p.T, p.r, p.sigma, p.M, (unsigned long long)paires_euro, (unsigned long long)paires_asia, (unsigned long long)graine, hw);
  std::fprintf(f, "  \"europeen\": {\"prix\": %.6f, \"erreur_type\": %.6f, \"ic95\": [%.6f, %.6f], \"black_scholes\": %.6f, \"ecart\": %.6f, \"ecart_en_erreurs_types\": %.3f},\n",
               E.prix, E.se, E.prix - 1.96 * E.se, E.prix + 1.96 * E.se, bs, E.prix - bs, zE);
  std::fprintf(f, "  \"asiatique_arithmetique\": {\"prix\": %.6f, \"erreur_type\": %.6f, \"ic95\": [%.6f, %.6f]},\n", A.prix, A.se, A.prix - 1.96 * A.se, A.prix + 1.96 * A.se);
  std::fprintf(f, "  \"asiatique_geometrique_validation\": {\"prix_mc\": %.6f, \"erreur_type\": %.6f, \"forme_fermee\": %.6f, \"ecart_en_erreurs_types\": %.3f},\n",
               G.prix, G.se, geo_exact, zG);
  std::fprintf(f, "  \"speedup\": [\n");
  for (size_t i = 0; i < lignes.size(); ++i) {
    const Ligne& l = lignes[i];
    std::fprintf(f, "    {\"threads\": %d, \"temps_europeen_s\": %.4f, \"speedup_europeen\": %.2f, \"temps_asiatique_s\": %.4f, \"speedup_asiatique\": %.2f, \"efficacite_asiatique\": %.2f}%s\n",
                 l.nt, l.t_e, lignes[0].t_e / l.t_e, l.t_a, lignes[0].t_a / l.t_a, lignes[0].t_a / l.t_a / l.nt, i + 1 < lignes.size() ? "," : "");
  }
  std::fprintf(f, "  ]\n}\n");
  std::fclose(f);
  std::printf("\n-> results/mc_pricer.json\n");
  return (std::fabs(zE) < 3 && std::fabs(zG) < 3) ? 0 : 1;
}
