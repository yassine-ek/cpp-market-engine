// Simulation multi-agents sur le carnet plat (b) :
//  - traders "zero-intelligence" (modèle de Santa Fe / Smith-Farmer) : arrivées Poisson d'ordres limites,
//    d'ordres au marché et d'annulations (taux par ordre vivant) ;
//  - un market maker Avellaneda-Stoikov (AS) qui recote toutes les L microsecondes SIMULÉES,
//    comparé à un market maker naïf à spread fixe et à l'absence de market maker.
// L'horloge est simulée (double, en microsecondes) : la "latence" L est la période de recotation,
// donc le délai pendant lequel le market maker subit un marché qui a bougé sans pouvoir réagir.
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "carnet.hpp"
#include "util.hpp"

using namespace marche;

constexpr Prix PREF = 10000;
constexpr double INF = std::numeric_limits<double>::infinity();
constexpr uint32_t NIL = 0xFFFFFFFFu;

struct ParamsZI {
  double lambda_limite = 8000;  // ordres limites par seconde (les deux côtés)
  double lambda_marche = 1500;  // ordres au marché par seconde
  double delta = 8;             // taux d'annulation PAR ordre vivant (/s) : c'est lui qui stabilise le carnet
  int bande = 40;               // profondeur max d'une limite sous le touch opposé (ticks)
  int taille_limite_max = 3;    // tailles ~ U{1..3} lots
  double p_taille_marche = 0.4; // taille marché = 1 + Geom(p) lots
  double persistance = 0.5;     // P(un ordre au marché a le même sens que le précédent) ; 0.5 = Santa Fe pur
  int init_par_cote = 100;
  double burn_s = 1.0;          // chauffe sans market maker avant la mesure
};
struct ParamsMM { Qte taille = 2; int q_max = 10; };  // taille cotée par côté, borne d'inventaire (lots)

enum class Type { Aucun, AS, Naif };
struct Strategie { Type type = Type::Aucun; double L_us = 0, gamma = 0, demi = 0; };
struct Calibrage { double sigma2 = 1, k = 1, sigma2_100ms = 1; };  // σ² (ticks²/s) et k (1/tick) du modèle AS
struct Resultat { double pnl = 0; int inv_max = 0; int nb_trades = 0; double spread_moyen = 0; };
struct Echant { double t_us; double mid; Prix bb, ba, mm_bid, mm_ask; int inv; double pnl; };

static const char* nom_type(Type t) { return t == Type::AS ? "AS" : t == Type::Naif ? "naif" : "sans_mm"; }

class Session {
 public:
  Session(const ParamsZI& z, const ParamsMM& m, const Calibrage& cal, double duree_s)
      : z_(z), m_(m), cal_(cal), duree_us_(duree_s * 1e6), book_(Config{PREF, 1024, 1 << 16, 1 << 20}) {}

  // trace : échantillonne l'état tous les dt_trace_us ; profondeurs : profondeur atteinte par chaque ordre au marché.
  Resultat lancer(const Strategie& s, uint64_t graine, std::vector<Echant>* trace = nullptr, double dt_trace_us = 1000,
                  std::vector<double>* profondeurs = nullptr) {
    reinit(graine);
    trace_ = trace;
    dt_trace_ = dt_trace_us;
    prof_ = profondeurs;
    const double t_burn = z_.burn_s * 1e6, t_fin = t_burn + duree_us_;
    t_fin_ = t_fin;
    next_samp_ = t_burn;

    for (int i = 0; i < 2 * z_.init_par_cote; ++i) ordre_limite((i & 1) ? Cote::Achat : Cote::Vente);
    t_zi_ = tirer_dt();
    while (t_zi_ < t_burn) {  // phase 1 : ZI seuls
      avancer(t_zi_);
      evt_zi();
      maj_spread();
      t_zi_ = t_ + tirer_dt();
    }
    avancer(t_burn);
    actif_ = true;

    double t_mm = s.type == Type::Aucun ? INF : t_burn;  // phase 2 : avec market maker
    for (;;) {
      const double te = std::min(t_zi_, t_mm);
      if (te >= t_fin) break;
      avancer(te);
      if (t_mm <= t_zi_) { requote(s); t_mm += s.L_us; }
      else { evt_zi(); t_zi_ = t_ + tirer_dt(); }
      maj_spread();
    }
    avancer(t_fin);

    Resultat r;
    r.pnl = cash_ + mm_.inv * mid_zi();  // inventaire final valorisé au mid, sans coût de liquidation
    r.inv_max = inv_max_;
    r.nb_trades = nb_fills_;
    r.spread_moyen = acc_dt_ > 0 ? acc_sp_ / acc_dt_ : 0;
    return r;
  }
  long violations() const { return violations_; }

 private:
  struct MM { Id id_bid = 0, id_ask = 0; Prix px_bid = 0, px_ask = 0; Qte rest_bid = 0, rest_ask = 0; int inv = 0; };

  void reinit(uint64_t graine) {
    rng_.seed(graine);
    book_.vider();
    live_.clear();
    std::fill(pos_.begin(), pos_.end(), NIL);
    mm_ = MM{};
    dernier_ = Cote::Achat;
    prochain_id_ = 1;
    cash_ = acc_sp_ = acc_dt_ = 0;
    inv_max_ = nb_fills_ = 0;
    t_ = 0;
    sp_ = 0;
    actif_ = false;
    last_mid_ = PREF;
  }

  double u01() { return static_cast<double>(rng_() >> 11) * 0x1.0p-53; }
  int ent(int n) { return static_cast<int>(rng_() % static_cast<uint64_t>(n)); }
  double tirer_dt() {  // temps jusqu'au prochain événement ZI (exponentielle), en µs
    const double taux = z_.lambda_limite + z_.lambda_marche + z_.delta * live_.size();
    return -std::log(1.0 - u01()) / taux * 1e6;
  }

  // Avance l'horloge : échantillonne la trajectoire et intègre le spread (état constant entre deux événements).
  void avancer(double te) {
    while (trace_ && next_samp_ <= te) { echantillonner(next_samp_); next_samp_ += dt_trace_; }
    if (actif_ && sp_ > 0 && te > t_) { acc_sp_ += sp_ * (te - t_); acc_dt_ += te - t_; }
    t_ = te;
  }
  void maj_spread() {
    const Prix bb = book_.meilleur(Cote::Achat), ba = book_.meilleur(Cote::Vente);
    sp_ = (bb && ba) ? ba - bb : 0;
  }
  void echantillonner(double t) {
    trace_->push_back({t - z_.burn_s * 1e6, mid_zi(), book_.meilleur(Cote::Achat), book_.meilleur(Cote::Vente), mm_.px_bid,
                       mm_.px_ask, mm_.inv, cash_ + mm_.inv * mid_zi()});
  }

  // ---- vue du marché SANS les ordres du market maker (sinon il coterait par rapport à lui-même)
  Prix meilleur_zi(Cote c) const {
    NiveauVue v[2];
    const int n = book_.profondeur(c, v, 2);
    for (int i = 0; i < n; ++i) {
      const int64_t propre = c == Cote::Achat ? (v[i].prix == mm_.px_bid ? mm_.rest_bid : 0)
                                              : (v[i].prix == mm_.px_ask ? mm_.rest_ask : 0);
      if (v[i].qte > propre) return v[i].prix;
    }
    return SANS_PRIX;
  }
  double mid_zi() {
    const Prix b = meilleur_zi(Cote::Achat), a = meilleur_zi(Cote::Vente);
    if (b && a) last_mid_ = 0.5 * (b + a);
    return last_mid_;  // un côté vide : on garde le dernier mid connu
  }

  // ---- traders zero-intelligence
  void enregistrer(Id id) {
    if (id >= pos_.size()) pos_.resize(std::max<size_t>(id + 1, pos_.size() * 2), NIL);
    pos_[id] = static_cast<uint32_t>(live_.size());
    live_.push_back(id);
  }
  void oublier(Id id) {
    if (id >= pos_.size() || pos_[id] == NIL) return;
    const uint32_t p = pos_[id];
    live_[p] = live_.back();
    pos_[live_[p]] = p;
    live_.pop_back();
    pos_[id] = NIL;
  }
  void ordre_limite(Cote c) {
    const Prix bb = book_.meilleur(Cote::Achat), ba = book_.meilleur(Cote::Vente);
    Prix p;
    // Jamais marchable : un acheteur se place sous le meilleur ask, un vendeur au-dessus du meilleur bid.
    if (c == Cote::Achat) p = (ba ? ba : (bb ? bb + 2 : PREF)) - 1 - ent(z_.bande);
    else p = (bb ? bb : (ba ? ba - 2 : PREF)) + 1 + ent(z_.bande);
    p = std::clamp<Prix>(p, PREF - 1000, PREF + 1000);
    const Id id = prochain_id_++;
    book_.ajouter_limite(id, c, p, 1 + ent(z_.taille_limite_max));
    if (book_.contient(id)) enregistrer(id);
  }
  void ordre_marche() {
    // Flux d'ordres persistant (chaîne de Markov sur le sens) : fait d'observation connu, et seule source
    // d'anti-sélection ici, puisque des ZI purs n'ont aucune information.
    const Cote c = u01() < z_.persistance ? dernier_ : oppose(dernier_);
    dernier_ = c;
    const double mid_avant = prof_ ? 0.5 * (book_.meilleur(Cote::Achat) + book_.meilleur(Cote::Vente)) : 0;
    const Qte q = 1 + std::geometric_distribution<int>(z_.p_taille_marche)(rng_);
    book_.ordre_marche(prochain_id_++, c, q);
    if (prof_ && !book_.trades().empty() && mid_avant > 0) prof_->push_back(std::abs(book_.trades().back().prix - mid_avant));
  }
  void annulation() {
    if (live_.empty()) return;
    const Id id = live_[ent(static_cast<int>(live_.size()))];
    book_.annuler(id);
    oublier(id);
  }
  void evt_zi() {
    const double rl = z_.lambda_limite, rm = z_.lambda_marche, rc = z_.delta * live_.size();
    const double r = u01() * (rl + rm + rc);
    if (r < rl) ordre_limite((rng_() & 1) ? Cote::Achat : Cote::Vente);
    else if (r < rl + rm) ordre_marche();
    else annulation();
    traiter_trades();
  }

  // Comptabilité des exécutions : fills du market maker, ou disparition d'un ordre ZI entièrement exécuté.
  void traiter_trades() {
    for (const Trade& t : book_.trades()) {
      if (t.maker == mm_.id_bid) {
        mm_.inv += t.qte; cash_ -= static_cast<double>(t.prix) * t.qte; mm_.rest_bid -= t.qte;
        if (mm_.rest_bid == 0) { mm_.id_bid = 0; mm_.px_bid = 0; }
        compter_fill();
      } else if (t.maker == mm_.id_ask) {
        mm_.inv -= t.qte; cash_ += static_cast<double>(t.prix) * t.qte; mm_.rest_ask -= t.qte;
        if (mm_.rest_ask == 0) { mm_.id_ask = 0; mm_.px_ask = 0; }
        compter_fill();
      } else if (!book_.contient(t.maker)) {
        oublier(t.maker);
      }
    }
    book_.vider_trades();
  }
  void compter_fill() {
    if (!actif_) return;
    ++nb_fills_;
    inv_max_ = std::max(inv_max_, std::abs(mm_.inv));
  }

  // ---- market maker
  bool inchange(Cote c, Prix cible) const {
    const Id id = c == Cote::Achat ? mm_.id_bid : mm_.id_ask;
    if (id == 0) return cible == 0;
    return (c == Cote::Achat ? mm_.px_bid : mm_.px_ask) == cible && (c == Cote::Achat ? mm_.rest_bid : mm_.rest_ask) == m_.taille;
  }
  void retirer_cote(Cote c) {
    Id& id = c == Cote::Achat ? mm_.id_bid : mm_.id_ask;
    if (id == 0) return;
    book_.annuler(id);
    id = 0;
    (c == Cote::Achat ? mm_.px_bid : mm_.px_ask) = 0;
    (c == Cote::Achat ? mm_.rest_bid : mm_.rest_ask) = 0;
  }
  void poser_cote(Cote c, Prix p) {
    if (p == 0) return;
    Id& id = c == Cote::Achat ? mm_.id_bid : mm_.id_ask;
    if (id != 0) return;  // côté inchangé
    id = prochain_id_++;
    book_.ajouter_limite(id, c, p, m_.taille);
    (c == Cote::Achat ? mm_.px_bid : mm_.px_ask) = p;
    (c == Cote::Achat ? mm_.rest_bid : mm_.rest_ask) = m_.taille;
    if (!book_.trades().empty()) { ++violations_; book_.vider_trades(); }  // ne doit jamais arriver : cotes passives
  }

  void requote(const Strategie& s) {
    const Prix bz = meilleur_zi(Cote::Achat), az = meilleur_zi(Cote::Vente);
    const double mid = mid_zi();
    const double tau = std::max(0.0, (t_fin_ - t_) * 1e-6);  // T - t, en secondes
    Prix b, a;
    if (s.type == Type::Naif) {
      // Spread fixe : h ticks de part et d'autre du mid arrondi. h est entier : un demi-spread réel serait
      // écrasé par l'arrondi au tick (deux valeurs voisines donneraient exactement les mêmes cotes).
      b = static_cast<Prix>(std::floor(mid)) - static_cast<Prix>(s.demi);
      a = static_cast<Prix>(std::ceil(mid)) + static_cast<Prix>(s.demi);
    } else {
      // r = s - q γ σ² (T-t) ; demi-spread = [γ σ² (T-t) + (2/γ) ln(1 + γ/k)] / 2
      const double centre = mid - mm_.inv * s.gamma * cal_.sigma2 * tau;
      const double demi = 0.5 * (s.gamma * cal_.sigma2 * tau + 2.0 / s.gamma * std::log1p(s.gamma / cal_.k));
      b = static_cast<Prix>(std::floor(centre - demi));
      a = static_cast<Prix>(std::ceil(centre + demi));
    }
    if (az && b >= az) b = az - 1;  // rester passif : ne jamais croiser les ordres des autres
    if (bz && a <= bz) a = bz + 1;
    if (a <= b) a = b + 1;
    b = std::max<Prix>(b, PREF - 1000);
    a = std::min<Prix>(a, PREF + 1000);
    // Bornes d'inventaire : on ne cote plus du côté qui aggraverait la position.
    const Prix cb = (mm_.inv + m_.taille <= m_.q_max) ? b : 0;
    const Prix ca = (mm_.inv - m_.taille >= -m_.q_max) ? a : 0;
    // On annule d'abord, on repose ensuite : sinon un nouveau bid pourrait croiser l'ancien ask.
    if (!inchange(Cote::Achat, cb)) retirer_cote(Cote::Achat);
    if (!inchange(Cote::Vente, ca)) retirer_cote(Cote::Vente);
    poser_cote(Cote::Achat, cb);
    poser_cote(Cote::Vente, ca);
  }

  ParamsZI z_;
  ParamsMM m_;
  Calibrage cal_;
  double duree_us_;
  CarnetPlat book_;
  std::mt19937_64 rng_;
  std::vector<Id> live_;
  std::vector<uint32_t> pos_;
  MM mm_;
  Id prochain_id_ = 1;
  Cote dernier_ = Cote::Achat;
  double cash_ = 0, t_ = 0, t_zi_ = 0, t_fin_ = 0, acc_sp_ = 0, acc_dt_ = 0, last_mid_ = PREF, sp_ = 0;
  int inv_max_ = 0, nb_fills_ = 0;
  bool actif_ = false;
  long violations_ = 0;
  std::vector<Echant>* trace_ = nullptr;
  std::vector<double>* prof_ = nullptr;
  double dt_trace_ = 1000, next_samp_ = 0;
};

// σ² : variance des variations du mid sur l'horizon moyen de la session (T/2), par seconde. Le mid des ZI se
// mean-revert (le carnet se remplit après chaque ordre au marché) : σ² mesuré à 100 ms vaut plus du double de
// σ² mesuré à 1 s, et l'AS surestimerait alors le risque d'inventaire. L'horizon doit donc être celui de (T-t).
// k : décroissance exponentielle de la profondeur atteinte par les ordres au marché.
static Calibrage calibrer(const ParamsZI& z, double horizon_s, double* spread_zi, double* nb_marche_s) {
  const double duree = 300.0, pas = 0.1;
  Session s(z, ParamsMM{}, Calibrage{}, duree);
  std::vector<Echant> tr;
  std::vector<double> prof;
  const Resultat r = s.lancer(Strategie{}, splitmix64(777), &tr, pas * 1e6, &prof);
  auto var_par_s = [&](size_t h) {  // incréments chevauchants sur h pas
    double m1 = 0, m2 = 0;
    size_t n = 0;
    for (size_t i = h; i < tr.size(); ++i) {
      const double d = tr[i].mid - tr[i - h].mid;
      m1 += d; m2 += d * d; ++n;
    }
    return (m2 / n - (m1 / n) * (m1 / n)) / (h * pas);
  };
  Calibrage c;
  c.sigma2 = var_par_s(static_cast<size_t>(std::lround(horizon_s / pas)));
  c.sigma2_100ms = var_par_s(1);
  // ln S(δ) = ln A - k δ , S(δ) = P(profondeur >= δ), régression sur la zone 2 % - 70 %
  std::vector<double> xs, ys;
  for (double d = 0.5; d < 60; d += 0.5) {
    const double S = static_cast<double>(std::count_if(prof.begin(), prof.end(), [&](double p) { return p >= d; })) / prof.size();
    if (S >= 0.02 && S <= 0.70) { xs.push_back(d); ys.push_back(std::log(S)); }
  }
  c.k = 0.5;
  if (xs.size() >= 3) {
    double mx = 0, my = 0, sxy = 0, sxx = 0;
    for (size_t i = 0; i < xs.size(); ++i) { mx += xs[i]; my += ys[i]; }
    mx /= xs.size(); my /= ys.size();
    for (size_t i = 0; i < xs.size(); ++i) { sxy += (xs[i] - mx) * (ys[i] - my); sxx += (xs[i] - mx) * (xs[i] - mx); }
    c.k = -sxy / sxx;
  }
  *spread_zi = r.spread_moyen;
  *nb_marche_s = prof.size() / duree;
  return c;
}

struct Config1 { Strategie s; };

int main(int argc, char** argv) {
  int N = 500, W = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
  uint64_t graine = 20260101;
  double duree = 2.0;
  for (int i = 1; i + 1 < argc; ++i) {
    if (!std::strcmp(argv[i], "--sessions")) N = std::stoi(argv[i + 1]);
    else if (!std::strcmp(argv[i], "--graine")) graine = std::stoull(argv[i + 1]);
    else if (!std::strcmp(argv[i], "--threads")) W = std::stoi(argv[i + 1]);
    else if (!std::strcmp(argv[i], "--duree")) duree = std::stod(argv[i + 1]);
  }
  std::filesystem::create_directories("results");
  const ParamsMM mm;
  const std::vector<double> regimes = {0.5, 0.75};  // persistance du sens des ordres au marché : Santa Fe pur, puis flux persistant
  const std::vector<double> Ls = {10, 100, 1000, 10000};
  const std::vector<double> gammas = {0.001, 0.005, 0.02};
  const std::vector<double> demis = {1, 2, 3};  // demi-spread du market maker naïf, en ticks

  FILE* f = std::fopen("results/sessions.csv", "w");
  if (!f) { std::perror("results/sessions.csv"); return 1; }
  std::fprintf(f, "persistance,strategie,L_us,gamma,demi_spread,session,pnl,inv_max,nb_trades,spread_moyen\n");
  FILE* meta = std::fopen("results/simulateur_meta.json", "w");
  std::fprintf(meta, "{\n  \"sessions_par_config\": %d, \"graine\": %llu, \"duree_session_s\": %.1f,\n", N, (unsigned long long)graine, duree);
  std::fprintf(meta, "  \"mm\": {\"taille\": %d, \"q_max\": %d, \"L_us\": [10, 100, 1000, 10000], \"gammas\": [0.001, 0.005, 0.02], \"demi_spreads_naif\": [1, 2, 3]},\n  \"regimes\": [\n", mm.taille, mm.q_max);

  double temps_total = 0;
  long viol_total = 0;
  Calibrage cal_pur;
  for (size_t ir = 0; ir < regimes.size(); ++ir) {
    ParamsZI z;
    z.persistance = regimes[ir];
    double spread_zi = 0, nb_marche_s = 0;
    const Calibrage cal = calibrer(z, duree / 2, &spread_zi, &nb_marche_s);
    if (ir == 0) cal_pur = cal;
    std::printf("\n=== persistance %.2f ===\ncalibration ZI : sigma2(horizon %.1f s)=%.1f  sigma2(100 ms)=%.1f ticks^2/s  k=%.3f /tick  spread ZI moyen=%.2f ticks  ordres marche/s=%.0f\n",
                z.persistance, duree / 2, cal.sigma2, cal.sigma2_100ms, cal.k, spread_zi, nb_marche_s);

    std::vector<Config1> cfgs;
    cfgs.push_back({{Type::Aucun, 0, 0, 0}});
    for (double L : Ls) {
      for (double g : gammas) cfgs.push_back({{Type::AS, L, g, 0}});
      for (double h : demis) cfgs.push_back({{Type::Naif, L, 0, h}});
    }

    // Chaque (config, session) est une tâche. Graine = f(session) seulement : toutes les stratégies partent des mêmes
    // aléas (comparaison appariée) et le résultat ne dépend pas du nombre de threads.
    const size_t nconf = cfgs.size(), njobs = nconf * static_cast<size_t>(N);
    std::vector<Resultat> res(njobs);
    std::atomic<size_t> prochain{0};
    std::atomic<long> violations{0};
    const auto t0 = std::chrono::steady_clock::now();
    {
      std::vector<std::thread> pool;
      for (int w = 0; w < W; ++w)
        pool.emplace_back([&] {
          Session s(z, mm, cal, duree);
          for (size_t j; (j = prochain.fetch_add(1)) < njobs;) {
            const size_t c = j / N, i = j % N;
            res[j] = s.lancer(cfgs[c].s, splitmix64(graine ^ splitmix64(i + 1)));
          }
          violations += s.violations();
        });
      for (auto& t : pool) t.join();
    }
    const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    temps_total += dt;
    viol_total += violations.load();
    std::printf("%zu sessions de %.1f s simulees en %.1f s (%d threads), violations de passivite : %ld\n", njobs, duree, dt, W,
                violations.load());
    std::printf("%-8s %7s %6s %6s | %9s %7s | %7s %7s %6s\n", "strat", "L_us", "gamma", "demi", "PnL moy", "ecart-t", "inv_max", "trades", "spread");
    for (size_t c = 0; c < nconf; ++c) {
      double sp = 0, sp2 = 0, si = 0, st = 0, ss = 0;
      const Strategie& s = cfgs[c].s;
      for (int i = 0; i < N; ++i) {
        const Resultat& r = res[c * N + i];
        std::fprintf(f, "%.2f,%s,%.0f,%g,%.3f,%d,%.2f,%d,%d,%.4f\n", z.persistance, nom_type(s.type), s.L_us, s.gamma, s.demi, i, r.pnl,
                     r.inv_max, r.nb_trades, r.spread_moyen);
        sp += r.pnl; sp2 += r.pnl * r.pnl; si += r.inv_max; st += r.nb_trades; ss += r.spread_moyen;
      }
      const double m = sp / N, sd = std::sqrt(std::max(0.0, sp2 / N - m * m));
      std::printf("%-8s %7.0f %6g %6.2f | %9.1f %7.1f | %7.1f %7.1f %6.2f\n", nom_type(s.type), s.L_us, s.gamma, s.demi, m, sd, si / N, st / N, ss / N);
    }
    std::fprintf(meta, "    {\"persistance\": %.2f, \"sigma2_ticks2_par_s\": %.2f, \"sigma2_100ms_ticks2_par_s\": %.2f, \"horizon_calibrage_sigma2_s\": %.1f, \"k_par_tick\": %.4f, \"spread_zi_moyen_ticks\": %.2f, \"ordres_marche_par_s\": %.0f}%s\n",
                 z.persistance, cal.sigma2, cal.sigma2_100ms, duree / 2, cal.k, spread_zi, nb_marche_s, ir + 1 < regimes.size() ? "," : "");
  }
  std::fclose(f);
  std::fprintf(meta, "  ],\n  \"temps_calcul_s\": %.1f, \"threads\": %d, \"violations_passivite\": %ld\n}\n", temps_total, W, viol_total);
  std::fclose(meta);

  // Trajectoire d'une session exemple (Santa Fe pur) : AS, L = 100 µs, γ central.
  ParamsZI zp;
  {
    Session s(zp, mm, cal_pur, duree);
    std::vector<Echant> tr;
    s.lancer(Strategie{Type::AS, 100, gammas[1], 0}, splitmix64(graine ^ splitmix64(1)), &tr, 200);
    FILE* g = std::fopen("results/trajectoire.csv", "w");
    std::fprintf(g, "t_us,mid_zi,meilleur_bid,meilleur_ask,mm_bid,mm_ask,inventaire,pnl_mtm\n");
    for (const Echant& e : tr)
      std::fprintf(g, "%.0f,%.1f,%d,%d,%d,%d,%d,%.1f\n", e.t_us, e.mid, e.bb, e.ba, e.mm_bid, e.mm_ask, e.inv, e.pnl);
    std::fclose(g);
    std::printf("\ntrajectoire exemple : %zu points -> results/trajectoire.csv\n", tr.size());
  }
  // Série de prix longue sans market maker : faits stylisés (rendements, autocorrélation des |rendements|).
  {
    Session s(zp, mm, cal_pur, 100.0);
    std::vector<Echant> tr;
    s.lancer(Strategie{}, splitmix64(graine ^ 0xFAB), &tr, 1000);
    FILE* g = std::fopen("results/serie_mid.csv", "w");
    std::fprintf(g, "t_us,mid\n");
    for (const Echant& e : tr) std::fprintf(g, "%.0f,%.1f\n", e.t_us, e.mid);
    std::fclose(g);
    std::printf("serie de prix : %zu points (pas 1 ms) -> results/serie_mid.csv\n", tr.size());
  }
  return 0;
}
