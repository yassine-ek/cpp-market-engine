// Benchmark : file SPSC lock-free VS std::mutex + std::queue, même charge, même code.
//
// Trois mesures, chacune répétée REPS fois (alternées SPSC / mutex), on publie la MÉDIANE :
//  1. débit de la file seule : un producteur pousse N entiers 64 bits, un consommateur les dépile ;
//  2. latence d'un saut (file seule) : le producteur émet à cadence fixe en estampillant le TSC,
//     le consommateur mesure l'écart à la réception ;
//  3. pipeline feed -> carnet -> stratégie : débit à plein régime, puis latence bout en bout
//     (émission du feed -> réception par la stratégie, deux files traversées) à cadence fixe.
// Horloge : rdtsc (TSC invariant, lu sur des cœurs différents : l'écart entre cœurs est supposé nul),
// calibré sur steady_clock. Estampille = heure PRÉVUE d'émission, donc si le producteur est bloqué par
// une file pleine, l'attente est comptée (pas d'« omission coordonnée »).
// Résultat : results/bench_spsc.json. Chiffres indicatifs : machine Windows non isolée.
#include <x86intrin.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "carnet.hpp"
#include "pipeline.hpp"

using namespace marche;
using Horloge = std::chrono::steady_clock;

static size_t N_DEBIT = 20000000;   // entiers pour le débit de la file seule
static size_t N_PIPE = 4000000;     // messages du pipeline à plein régime
static size_t N_LAT = 1000000;      // messages pour les mesures de latence
// Cadence des mesures de latence : DOIT rester sous le débit maximal du pipeline mutex (~0,8 Mmsg/s mesuré),
// sinon sa file se remplit et la « latence » mesurée n'est plus que la durée de remplissage de la file.
static double CADENCE = 3.0e5;      // messages/s
static int REPS = 5;
static size_t CAPACITE = 1024;
static constexpr uint64_t GRAINE = 20261013;

static double calibrer_ticks_par_s() {
  const auto t0 = Horloge::now();
  const uint64_t c0 = __rdtsc();
  while (Horloge::now() - t0 < std::chrono::milliseconds(300)) {}
  const uint64_t c1 = __rdtsc();
  const auto t1 = Horloge::now();
  return static_cast<double>(c1 - c0) / std::chrono::duration<double>(t1 - t0).count();
}

struct Perc { double p50, p99, p999, max, moy; };

static Perc percentiles(std::vector<uint64_t>& v, double ticks_par_ns, size_t ignorer) {
  if (v.size() > ignorer) v.erase(v.begin(), v.begin() + static_cast<long>(ignorer));  // chauffe : caches, pages
  std::sort(v.begin(), v.end());
  auto q = [&](double p) { return static_cast<double>(v[std::min(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size())))]) / ticks_par_ns; };
  double s = 0;
  for (uint64_t x : v) s += static_cast<double>(x);
  return {q(0.5), q(0.99), q(0.999), static_cast<double>(v.back()) / ticks_par_ns, s / static_cast<double>(v.size()) / ticks_par_ns};
}

static double mediane(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

// ---------------------------------------------------------------- 1. débit file seule
template <class File>
static double debit_file_seule(const int* coeurs) {
  File q(CAPACITE);
  std::atomic<bool> go{false};
  Horloge::time_point t_fin;
  std::thread prod([&] {
    epingler(coeurs[0]);
    while (!go.load(std::memory_order_acquire)) attendre();
    for (uint64_t i = 0; i < N_DEBIT; ++i)
      while (!q.try_push(i)) attendre();
  });
  uint64_t somme = 0;
  std::thread cons([&] {
    epingler(coeurs[1]);
    uint64_t v;
    for (uint64_t i = 0; i < N_DEBIT; ++i) {
      while (!q.try_pop(v)) attendre();
      somme += v;
    }
    t_fin = Horloge::now();
  });
  const auto t0 = Horloge::now();
  go.store(true, std::memory_order_release);
  prod.join();
  cons.join();
  if (somme != N_DEBIT * (N_DEBIT - 1) / 2) std::printf("ATTENTION : somme de controle fausse\n");
  return static_cast<double>(N_DEBIT) / std::chrono::duration<double>(t_fin - t0).count();
}

// ---------------------------------------------------------------- 2. latence d'un saut
template <class File>
static Perc latence_saut(const int* coeurs, double tps, double tpns) {
  File q(CAPACITE);
  std::atomic<bool> go{false};
  std::vector<uint64_t> lat;
  lat.reserve(N_LAT);
  const double pas = tps / CADENCE;
  std::thread prod([&] {
    epingler(coeurs[0]);
    while (!go.load(std::memory_order_acquire)) attendre();
    const uint64_t t0 = __rdtsc();
    for (uint64_t i = 0; i < N_LAT; ++i) {
      const uint64_t cible = t0 + static_cast<uint64_t>(static_cast<double>(i) * pas);
      while (__rdtsc() < cible) attendre();
      while (!q.try_push(cible)) attendre();
    }
  });
  std::thread cons([&] {
    epingler(coeurs[1]);
    uint64_t t;
    for (uint64_t i = 0; i < N_LAT; ++i) {
      while (!q.try_pop(t)) attendre();
      lat.push_back(__rdtsc() - t);
    }
  });
  go.store(true, std::memory_order_release);
  prod.join();
  cons.join();
  return percentiles(lat, tpns, N_LAT / 10);
}

// ---------------------------------------------------------------- 3. pipeline
struct ResPipe { double debit; Perc lat; bool conforme; uint64_t empreinte; };

template <template <class> class File>
static ResPipe pipeline(const std::vector<MsgOrdre>& ops_debit, const std::vector<MsgOrdre>& ops_lat, const Config& cfg,
                        const int* coeurs, double tps, double tpns, const ResultatPipeline& ref_debit,
                        const ResultatPipeline& ref_lat) {
  OptPipeline o;
  o.capacite = CAPACITE;
  o.ticks_par_s = tps;
  for (int i = 0; i < 3; ++i) o.coeurs[i] = coeurs[i];
  ResPipe r{};
  {  // plein régime
    ResultatPipeline x = executer_pipeline<File>(ops_debit, cfg, o);
    r.debit = static_cast<double>(ops_debit.size()) / x.duree_s;
    r.conforme = x.strat == ref_debit.strat;
    r.empreinte = x.strat.empreinte;
  }
  {  // cadencé, latences
    o.cadence_msg_s = CADENCE;
    o.latences = true;
    ResultatPipeline x = executer_pipeline<File>(ops_lat, cfg, o);
    r.conforme = r.conforme && x.strat == ref_lat.strat && x.lat_ticks.size() == ops_lat.size();
    r.lat = percentiles(x.lat_ticks, tpns, ops_lat.size() / 10);
  }
  return r;
}

static void ecrire_perc(FILE* f, const char* nom, const std::vector<Perc>& v) {
  auto med = [&](double Perc::*m) {
    std::vector<double> x;
    for (const Perc& p : v) x.push_back(p.*m);
    return mediane(x);
  };
  std::fprintf(f, "      \"%s\": {\"p50_ns\": %.1f, \"p99_ns\": %.1f, \"p999_ns\": %.1f, \"max_ns\": %.0f, \"moy_ns\": %.1f,\n", nom,
               med(&Perc::p50), med(&Perc::p99), med(&Perc::p999), med(&Perc::max), med(&Perc::moy));
  std::fprintf(f, "        \"p50_par_repetition\": [");
  for (size_t i = 0; i < v.size(); ++i) std::fprintf(f, "%s%.1f", i ? ", " : "", v[i].p50);
  std::fprintf(f, "], \"p99_par_repetition\": [");
  for (size_t i = 0; i < v.size(); ++i) std::fprintf(f, "%s%.1f", i ? ", " : "", v[i].p99);
  std::fprintf(f, "]}");
}

static void ecrire_debits(FILE* f, const char* nom, const std::vector<double>& v) {
  std::fprintf(f, "      \"%s\": {\"median_msg_s\": %.0f, \"par_repetition\": [", nom, mediane(v));
  for (size_t i = 0; i < v.size(); ++i) std::fprintf(f, "%s%.0f", i ? ", " : "", v[i]);
  std::fprintf(f, "]}");
}

int main(int argc, char** argv) {
  int coeurs[3] = {4, 6, 8};  // trois cœurs logiques pairs : sur un CPU SMT, des cœurs physiques distincts (hypothèse)
  bool epingle = true;
  for (int i = 1; i < argc; ++i) {
    const bool a2 = i + 1 < argc;
    if (!std::strcmp(argv[i], "--reps") && a2) REPS = std::stoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--cadence") && a2) CADENCE = std::stod(argv[++i]);
    else if (!std::strcmp(argv[i], "--capacite") && a2) CAPACITE = std::stoull(argv[++i]);
    else if (!std::strcmp(argv[i], "--n-debit") && a2) N_DEBIT = std::stoull(argv[++i]);
    else if (!std::strcmp(argv[i], "--n-pipe") && a2) N_PIPE = std::stoull(argv[++i]);
    else if (!std::strcmp(argv[i], "--n-lat") && a2) N_LAT = std::stoull(argv[++i]);
    else if (!std::strcmp(argv[i], "--sans-epinglage")) epingle = false;
    else if (!std::strcmp(argv[i], "--coeurs") && a2) std::sscanf(argv[++i], "%d,%d,%d", &coeurs[0], &coeurs[1], &coeurs[2]);
  }
  if (!epingle) coeurs[0] = coeurs[1] = coeurs[2] = -1;
#ifdef _WIN32
  SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
#endif

  std::printf("calibration horloge...\n");
  const double tps = calibrer_ticks_par_s(), tpns = tps * 1e-9;
  const unsigned hw = std::thread::hardware_concurrency();
  std::printf("TSC %.3f GHz, %u threads materiels, coeurs %d,%d,%d, cadence %.0f msg/s, capacite %zu, %d repetitions\n",
              tps * 1e-9, hw, coeurs[0], coeurs[1], coeurs[2], CADENCE, CAPACITE, REPS);

  const Config cfg{10000, 2048, 1u << 20, static_cast<uint32_t>(std::max(N_PIPE, N_LAT) + 16)};
  const std::vector<MsgOrdre> ops_debit = generer_ordres(N_PIPE, GRAINE, cfg), ops_lat = generer_ordres(N_LAT, GRAINE + 1, cfg);
  const ResultatPipeline ref_debit = reference_mono(ops_debit, cfg), ref_lat = reference_mono(ops_lat, cfg);
  std::printf("flux : %zu + %zu messages, %llu trades (debit), carnet final %zu ordres\n", N_PIPE, N_LAT,
              (unsigned long long)ref_debit.nb_trades, ref_debit.carnet->nb_ordres());

  std::vector<double> d_seul[2], d_pipe[2];
  std::vector<Perc> l_saut[2], l_pipe[2];
  bool conforme = true;
  for (int rep = 0; rep < REPS; ++rep) {
    std::printf("repetition %d/%d...\n", rep + 1, REPS);
    d_seul[0].push_back(debit_file_seule<FileSpsc<uint64_t>>(coeurs));
    d_seul[1].push_back(debit_file_seule<FileMutex<uint64_t>>(coeurs));
    l_saut[0].push_back(latence_saut<FileSpsc<uint64_t>>(coeurs, tps, tpns));
    l_saut[1].push_back(latence_saut<FileMutex<uint64_t>>(coeurs, tps, tpns));
    const ResPipe a = pipeline<FileSpsc>(ops_debit, ops_lat, cfg, coeurs, tps, tpns, ref_debit, ref_lat);
    const ResPipe b = pipeline<FileMutex>(ops_debit, ops_lat, cfg, coeurs, tps, tpns, ref_debit, ref_lat);
    conforme = conforme && a.conforme && b.conforme;
    d_pipe[0].push_back(a.debit);
    d_pipe[1].push_back(b.debit);
    l_pipe[0].push_back(a.lat);
    l_pipe[1].push_back(b.lat);
    std::printf("  file seule : SPSC %.1f / mutex %.1f Mmsg/s ; pipeline : SPSC %.2f / mutex %.2f Mmsg/s ; p50 pipeline %.0f / %.0f ns\n",
                d_seul[0].back() * 1e-6, d_seul[1].back() * 1e-6, a.debit * 1e-6, b.debit * 1e-6, a.lat.p50, b.lat.p50);
  }
  std::printf("pipelines conformes a la reference mono-thread : %s\n", conforme ? "oui" : "NON");

  FILE* f = std::fopen("results/bench_spsc.json", "w");
  if (!f) { std::printf("impossible d'ecrire results/bench_spsc.json\n"); return 1; }
  std::fprintf(f, "{\n  \"config\": {\"n_debit_file_seule\": %zu, \"n_pipeline_debit\": %zu, \"n_latence\": %zu, \"cadence_msg_s\": %.0f,\n"
                  "    \"capacite_files\": %zu, \"repetitions\": %d, \"ticks_par_ns\": %.4f, \"threads_materiels\": %u,\n"
                  "    \"coeurs_epingles\": [%d, %d, %d], \"priorite_processus\": \"haute (Windows)\", \"graine\": %llu,\n"
                  "    \"pipelines_conformes_a_la_reference_mono_thread\": %s, \"flags\": \"-O3 -march=native -std=c++20\"},\n",
               N_DEBIT, N_PIPE, N_LAT, CADENCE, CAPACITE, REPS, tpns, hw, coeurs[0], coeurs[1], coeurs[2],
               (unsigned long long)GRAINE, conforme ? "true" : "false");
  const char* noms[2] = {"spsc_lockfree", "mutex_queue"};
  std::fprintf(f, "  \"debit\": {\n");
  for (int k = 0; k < 2; ++k) {
    std::fprintf(f, "    \"%s\": {\n", noms[k]);
    ecrire_debits(f, "file_seule", d_seul[k]);
    std::fprintf(f, ",\n");
    ecrire_debits(f, "pipeline_3_threads", d_pipe[k]);
    std::fprintf(f, "\n    }%s\n", k ? "" : ",");
  }
  std::fprintf(f, "  },\n  \"latence\": {\n");
  for (int k = 0; k < 2; ++k) {
    std::fprintf(f, "    \"%s\": {\n", noms[k]);
    ecrire_perc(f, "un_saut_file_seule", l_saut[k]);
    std::fprintf(f, ",\n");
    ecrire_perc(f, "pipeline_bout_en_bout", l_pipe[k]);
    std::fprintf(f, "\n    }%s\n", k ? "" : ",");
  }
  std::fprintf(f, "  }\n}\n");
  std::fclose(f);
  std::printf("-> results/bench_spsc.json\n");
  return conforme ? 0 : 1;
}
