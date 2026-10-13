// Pipeline à 3 threads : feed handler -> carnet -> stratégie, reliés par deux files SPSC.
//
//   [feed] --MsgOrdre--> file 1 --> [carnet plat] --MsgTop--> file 2 --> [stratégie]
//
//  - feed      : rejoue une séquence d'ordres pré-générée (déterministe) ; en mode « cadencé » il
//                émet à débit fixe et estampille le message avec son heure PRÉVUE d'émission (si la
//                file est pleine et que le feed prend du retard, ce retard compte dans la latence).
//  - carnet    : applique chaque ordre au CarnetPlat et publie le top-of-book après CHAQUE message
//                (pas de conflation : un message en entrée = un message en sortie, ce qui permet de
//                mesurer la latence de tous les messages et de vérifier l'ordre bout en bout).
//  - stratégie : lit le top-of-book, compte les changements de mid et les spreads serrés, et
//                calcule une empreinte de toute la séquence reçue.
// Le pipeline est un template sur le type de file (FileSpsc ou FileMutex) : même code, même charge.
#pragma once
#include <x86intrin.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <random>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "carnet.hpp"
#include "file_mutex.hpp"
#include "spsc.hpp"

namespace marche {

enum TypeMsg : uint8_t { M_AJOUT = 0, M_ANNUL = 1, M_MARCHE = 2, M_FIN = 3 };

struct MsgOrdre {  // 24 octets
  uint8_t type, cote;
  Id id;
  Prix prix;
  Qte qte;
  uint64_t t_emis;  // ticks TSC de l'émission (0 si non mesuré)
};

struct MsgTop {  // 32 octets
  Prix bid, ask;  // SANS_PRIX (0) si le côté est vide
  uint32_t fin;
  uint64_t seq;     // numéro du message d'ordre qui a produit ce top
  uint64_t t_emis;  // recopié du message d'ordre : latence bout en bout
};

// Séquence synthétique déterministe : 70 % ajouts (10 % de limites marchables), 25 % annulations
// d'ordres récents (certaines visent un ordre déjà exécuté : le carnet les rejette, c'est voulu),
// 5 % ordres au marché. Le mid dérive lentement. Ce générateur est volontairement plus simple
// que celui du bench du carnet (qui pilote un carnet de référence) : ici seul compte d'avoir un
// flux reproductible qui exerce toutes les opérations.
inline std::vector<MsgOrdre> generer_ordres(size_t n, uint64_t graine, const Config& cfg = {}) {
  std::mt19937_64 rng(graine);
  std::geometric_distribution<int> geo(0.15);
  std::vector<MsgOrdre> v;
  v.reserve(n);
  Prix mid = cfg.prix_ref;
  Id prochain = 1;
  for (size_t k = 0; k < n; ++k) {
    if ((rng() & 7) == 0) mid = std::clamp<Prix>(mid + ((rng() & 1) ? 1 : -1), cfg.prix_ref - 500, cfg.prix_ref + 500);
    const uint32_t u = static_cast<uint32_t>(rng() % 100);
    MsgOrdre m{};
    m.cote = static_cast<uint8_t>(rng() & 1);
    const int signe = m.cote == 0 ? -1 : 1;  // achat sous le mid, vente au-dessus
    if (u < 70 || prochain == 1) {
      m.type = M_AJOUT;
      m.id = prochain++;
      m.prix = (rng() % 10 == 0) ? mid - signe * static_cast<Prix>(rng() % 3)  // marchable
                                 : mid + signe * (1 + geo(rng));              // passive
      m.qte = 1 + static_cast<Qte>(rng() % 100);
    } else if (u < 95) {
      m.type = M_ANNUL;
      const Id bas = prochain > 20000 ? prochain - 20000 : 1;
      m.id = bas + static_cast<Id>(rng() % (prochain - bas));
    } else {
      m.type = M_MARCHE;
      m.id = prochain++;
      m.qte = 1 + static_cast<Qte>(rng() % 700);  // assez gros pour que le carnet reste borné
    }
    v.push_back(m);
  }
  return v;
}

// Applique un message d'ordre au carnet et vide le journal de trades (renvoie leur nombre).
inline size_t appliquer(CarnetPlat& c, const MsgOrdre& m) {
  const Cote cote = static_cast<Cote>(m.cote);
  switch (m.type) {
    case M_AJOUT: c.ajouter_limite(m.id, cote, m.prix, m.qte); break;
    case M_ANNUL: c.annuler(m.id); break;
    default: c.ordre_marche(m.id, cote, m.qte); break;
  }
  const size_t nt = c.trades().size();
  c.vider_trades();
  return nt;
}

// État de la stratégie : tout ce qu'elle « voit » est résumé ici, pour comparer deux exécutions.
struct EtatStrategie {
  uint64_t nb_msgs = 0, nb_hausses = 0, nb_baisses = 0, nb_spread_serre = 0, empreinte = 0;
  int64_t dernier_mid2 = 0;  // 2 * mid, pour rester entier
  bool operator==(const EtatStrategie&) const = default;

  void voir(const MsgTop& t) {
    ++nb_msgs;
    if (t.bid != SANS_PRIX && t.ask != SANS_PRIX) {
      const int64_t mid2 = int64_t(t.bid) + t.ask;
      if (dernier_mid2 != 0) {
        if (mid2 > dernier_mid2) ++nb_hausses;
        else if (mid2 < dernier_mid2) ++nb_baisses;
      }
      dernier_mid2 = mid2;
      if (t.ask - t.bid <= 1) ++nb_spread_serre;  // « signal » : carnet serré
    }
    // empreinte dépendante de l'ordre : une perte, un doublon ou une permutation la change
    empreinte = (empreinte ^ (uint64_t(uint32_t(t.bid)) << 32 | uint32_t(t.ask)) ^ t.seq) * 0x9E3779B97F4A7C15ULL;
    empreinte ^= empreinte >> 29;
  }
};

struct ResultatPipeline {
  std::unique_ptr<CarnetPlat> carnet;
  EtatStrategie strat;
  uint64_t nb_trades = 0;
  double duree_s = 0;
  std::vector<uint64_t> lat_ticks;  // latence bout en bout par message (mode cadencé)
};

// Référence mono-thread : même séquence, mêmes fonctions, sans file.
inline ResultatPipeline reference_mono(const std::vector<MsgOrdre>& ops, const Config& cfg) {
  ResultatPipeline r;
  r.carnet = std::make_unique<CarnetPlat>(cfg);
  for (size_t i = 0; i < ops.size(); ++i) {
    r.nb_trades += appliquer(*r.carnet, ops[i]);
    r.strat.voir(MsgTop{r.carnet->meilleur(Cote::Achat), r.carnet->meilleur(Cote::Vente), 0, i, 0});
  }
  return r;
}

struct OptPipeline {
  size_t capacite = 1024;
  double cadence_msg_s = 0;      // 0 : le feed va aussi vite que possible
  double ticks_par_s = 0;        // requis si cadence > 0
  bool latences = false;         // enregistre la latence bout en bout (exige cadence > 0)
  int coeurs[3] = {-1, -1, -1};  // -1 : pas d'épinglage (SetThreadAffinityMask sous Windows)
};

inline void epingler(int coeur) {
#ifdef _WIN32
  if (coeur >= 0) SetThreadAffinityMask(GetCurrentThread(), 1ULL << coeur);
#else
  (void)coeur;
#endif
}

// Attente active. Sous -DATTENTE_COOPERATIVE (utilisé par build.sh pour les TESTS quand la machine a
// moins de 4 cœurs, ex. un runner CI) on cède le processeur : sinon 3 threads qui tournent à vide
// sur 2 cœurs n'avancent qu'à la cadence de l'ordonnanceur. Le benchmark n'utilise jamais ce mode.
#ifdef ATTENTE_COOPERATIVE
inline void attendre() { std::this_thread::yield(); }
#else
inline void attendre() { _mm_pause(); }
#endif

template <template <class> class File>
ResultatPipeline executer_pipeline(const std::vector<MsgOrdre>& ops, const Config& cfg, const OptPipeline& opt) {
  using Horloge = std::chrono::steady_clock;
  File<MsgOrdre> f1(opt.capacite);
  File<MsgTop> f2(opt.capacite);
  ResultatPipeline r;
  r.carnet = std::make_unique<CarnetPlat>(cfg);
  if (opt.latences) r.lat_ticks.reserve(ops.size());
  std::atomic<bool> go{false};
  Horloge::time_point t_fin;
  const double pas = opt.cadence_msg_s > 0 ? opt.ticks_par_s / opt.cadence_msg_s : 0;  // ticks entre deux émissions

  std::thread feed([&] {
    epingler(opt.coeurs[0]);
    while (!go.load(std::memory_order_acquire)) attendre();
    const uint64_t t0 = __rdtsc();
    for (size_t i = 0; i < ops.size(); ++i) {
      MsgOrdre m = ops[i];
      if (pas > 0) {
        const uint64_t cible = t0 + static_cast<uint64_t>(static_cast<double>(i) * pas);
        while (__rdtsc() < cible) attendre();
        m.t_emis = cible;
      }
      while (!f1.try_push(m)) attendre();
    }
    MsgOrdre fin{};
    fin.type = M_FIN;
    while (!f1.try_push(fin)) attendre();
  });

  std::thread moteur([&] {
    epingler(opt.coeurs[1]);
    CarnetPlat& c = *r.carnet;
    uint64_t seq = 0;
    MsgOrdre m;
    for (;;) {
      while (!f1.try_pop(m)) attendre();
      MsgTop t{};
      if (m.type == M_FIN) {
        t.fin = 1;
      } else {
        r.nb_trades += appliquer(c, m);
        t = MsgTop{c.meilleur(Cote::Achat), c.meilleur(Cote::Vente), 0, seq++, m.t_emis};
      }
      while (!f2.try_push(t)) attendre();
      if (t.fin) break;
    }
  });

  std::thread strategie([&] {
    epingler(opt.coeurs[2]);
    MsgTop t;
    for (;;) {
      while (!f2.try_pop(t)) attendre();
      if (t.fin) break;
      if (opt.latences) r.lat_ticks.push_back(__rdtsc() - t.t_emis);
      r.strat.voir(t);
    }
    t_fin = Horloge::now();
  });

  const auto t0 = Horloge::now();
  go.store(true, std::memory_order_release);
  feed.join();
  moteur.join();
  strategie.join();
  r.duree_s = std::chrono::duration<double>(t_fin - t0).count();
  return r;
}

}  // namespace marche
