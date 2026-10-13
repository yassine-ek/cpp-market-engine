#include "carnet.hpp"

#include <algorithm>
#include <bit>
#include <climits>
#include <iterator>

namespace marche {

static inline int idx(Cote c) { return static_cast<int>(c); }

// =====================================================================
// (a) CarnetMap : std::map<prix, std::list> — la version "manuel de cours"
// =====================================================================

CarnetMap::CarnetMap(const Config& cfg) : cfg_(cfg) { index_.reserve(cfg.ordres_max); }

void CarnetMap::vider() {
  achats_.clear();
  ventes_.clear();
  index_.clear();
  total_[0] = total_[1] = 0;
  trades_.clear();
}

// Consomme le côté opposé tant que le prix croise. Le trade se fait au prix du maker.
template <class M, class Croise>
Qte CarnetMap::apparier(M& opp, int64_t& total_opp, Id taker, Cote ct, Qte qte, Croise croise) {
  while (qte > 0 && !opp.empty()) {
    auto mi = opp.begin();
    if (!croise(mi->first)) break;
    Niveau& niv = mi->second;
    while (qte > 0 && !niv.file.empty()) {
      Ordre& m = niv.file.front();
      const Qte x = std::min(qte, m.qte);
      trades_.push_back({m.id, taker, mi->first, x, ct});
      m.qte -= x;
      niv.total -= x;
      total_opp -= x;
      qte -= x;
      if (m.qte == 0) {
        index_.erase(m.id);
        niv.file.pop_front();
      }
    }
    if (niv.file.empty()) opp.erase(mi);
  }
  return qte;
}

void CarnetMap::poser(Id id, Cote c, Prix p, Qte q) {
  auto enfiler = [&](Niveau& niv) {
    niv.file.push_back({id, q});
    niv.total += q;
    index_[id] = {c, p, std::prev(niv.file.end())};
  };
  if (c == Cote::Achat) enfiler(achats_[p]); else enfiler(ventes_[p]);
  total_[idx(c)] += q;
}

void CarnetMap::retirer(const Loc& l) {
  auto retirer_de = [&](auto& m) {
    auto mi = m.find(l.prix);
    Niveau& niv = mi->second;
    niv.total -= l.it->qte;
    total_[idx(l.cote)] -= l.it->qte;
    niv.file.erase(l.it);
    if (niv.file.empty()) m.erase(mi);
  };
  if (l.cote == Cote::Achat) retirer_de(achats_); else retirer_de(ventes_);
}

bool CarnetMap::ajouter_limite(Id id, Cote cote, Prix prix, Qte qte) {
  if (qte <= 0 || id >= ID_MAX || !cfg_.valide(prix) || index_.count(id)) return false;
  Qte reste;
  if (cote == Cote::Achat)
    reste = apparier(ventes_, total_[1], id, cote, qte, [prix](Prix p) { return p <= prix; });
  else
    reste = apparier(achats_, total_[0], id, cote, qte, [prix](Prix p) { return p >= prix; });
  if (reste > 0) poser(id, cote, prix, reste);
  return true;
}

bool CarnetMap::annuler(Id id) {
  auto it = index_.find(id);
  if (it == index_.end()) return false;
  const Loc l = it->second;
  index_.erase(it);
  retirer(l);
  return true;
}

bool CarnetMap::modifier(Id id, Prix np, Qte nq) {
  auto it = index_.find(id);
  if (it == index_.end() || nq <= 0) return false;
  const Loc l = it->second;
  if (np == l.prix) {
    if (nq == l.it->qte) return true;
    if (nq < l.it->qte) {  // baisse : on garde sa place dans la file
      const Qte delta = l.it->qte - nq;
      l.it->qte = nq;
      total_[idx(l.cote)] -= delta;
      if (l.cote == Cote::Achat) achats_.find(np)->second.total -= delta;
      else ventes_.find(np)->second.total -= delta;
      return true;
    }
    // hausse : fin de file. Pas d'appariement possible, l'ordre reposait déjà sans croiser.
    index_.erase(it);
    retirer(l);
    poser(id, l.cote, np, nq);
    return true;
  }
  if (!cfg_.valide(np)) return false;
  index_.erase(it);
  retirer(l);
  return ajouter_limite(id, l.cote, np, nq);  // perd la priorité, peut croiser
}

Qte CarnetMap::ordre_marche(Id id, Cote cote, Qte qte) {
  if (qte <= 0) return 0;
  Qte reste;
  if (cote == Cote::Achat)
    reste = apparier(ventes_, total_[1], id, cote, qte, [](Prix) { return true; });
  else
    reste = apparier(achats_, total_[0], id, cote, qte, [](Prix) { return true; });
  return qte - reste;
}

Qte CarnetMap::qte_ordre(Id id) const {
  auto it = index_.find(id);
  return it == index_.end() ? 0 : it->second.it->qte;
}

Prix CarnetMap::meilleur(Cote c) const {
  if (c == Cote::Achat) return achats_.empty() ? SANS_PRIX : achats_.begin()->first;
  return ventes_.empty() ? SANS_PRIX : ventes_.begin()->first;
}

int64_t CarnetMap::qte_niveau(Cote c, Prix p) const {
  if (c == Cote::Achat) {
    auto it = achats_.find(p);
    return it == achats_.end() ? 0 : it->second.total;
  }
  auto it = ventes_.find(p);
  return it == ventes_.end() ? 0 : it->second.total;
}

template <class M>
static int profondeur_map(const M& m, NiveauVue* out, int n) {
  int i = 0;
  for (const auto& [p, niv] : m) {
    if (i >= n) break;
    out[i++] = {p, niv.total};
  }
  return i;
}

int CarnetMap::profondeur(Cote c, NiveauVue* out, int n) const {
  return c == Cote::Achat ? profondeur_map(achats_, out, n) : profondeur_map(ventes_, out, n);
}

// =====================================================================
// (b) CarnetPlat : tableau indexé par tick + pool d'ordres contigu
// =====================================================================

// Plus grand indice <= i dont le bit est à 1 (-1 si aucun).
static int bit_bas(const std::vector<uint64_t>& b, int i) {
  if (i < 0) return -1;
  int w = i >> 6;
  uint64_t m = b[w] & (~0ULL >> (63 - (i & 63)));
  for (;;) {
    if (m) return (w << 6) + 63 - std::countl_zero(m);
    if (--w < 0) return -1;
    m = b[w];
  }
}

// Plus petit indice >= i dont le bit est à 1 (-1 si aucun).
static int bit_haut(const std::vector<uint64_t>& b, int i, int nb_niv) {
  if (i >= nb_niv) return -1;
  int w = i >> 6;
  const int nw = static_cast<int>(b.size());
  uint64_t m = b[w] & (~0ULL << (i & 63));
  for (;;) {
    if (m) return (w << 6) + std::countr_zero(m);
    if (++w >= nw) return -1;
    m = b[w];
  }
}

CarnetPlat::CarnetPlat(const Config& cfg) : cfg_(cfg) {
  base_ = cfg.prix_ref - cfg.demi_largeur;
  nb_niv_ = (2 * cfg.demi_largeur + 1 + 63) / 64 * 64;
  for (int s = 0; s < 2; ++s) {
    niv_[s].assign(nb_niv_, Niveau{});
    bits_[s].assign(nb_niv_ / 64, 0);
  }
  slots_.reserve(cfg.ordres_max);  // évite toute réallocation en régime établi
  pos_.assign(cfg.ids_max, NIL);
}

void CarnetPlat::vider() {
  for (int s = 0; s < 2; ++s) {
    std::fill(niv_[s].begin(), niv_[s].end(), Niveau{});
    std::fill(bits_[s].begin(), bits_[s].end(), 0);
    best_[s] = -1;
    total_[s] = 0;
  }
  libre_ = NIL;
  for (const Slot& x : slots_) pos_[x.id] = NIL;  // O(ordres vus), pas O(table d'ids)
  slots_.clear();
  nb_vivants_ = 0;
  trades_.clear();
}

uint32_t CarnetPlat::allouer() {
  if (libre_ != NIL) {
    const uint32_t s = libre_;
    libre_ = slots_[s].next;
    return s;
  }
  slots_.emplace_back();
  return static_cast<uint32_t>(slots_.size() - 1);
}

int CarnetPlat::prochain(int s, int i) const {
  return s == 0 ? bit_bas(bits_[0], i - 1) : bit_haut(bits_[1], i + 1, nb_niv_);
}

// Ajoute en fin de file du niveau idx : c'est ce qui fait la priorité temps.
void CarnetPlat::poser(Id id, int s, int i, Qte q) {
  const uint32_t sl = allouer();  // peut réallouer slots_ : aucune référence gardée avant
  Niveau& n = niv_[s][i];
  slots_[sl] = {id, q, n.queue, NIL, i, static_cast<Cote>(s)};
  if (n.queue != NIL) {
    slots_[n.queue].next = sl;
  } else {
    n.tete = sl;
    bits_[s][i >> 6] |= 1ULL << (i & 63);
  }
  n.queue = sl;
  n.total += q;
  total_[s] += q;
  pos_[id] = sl;
  ++nb_vivants_;
  if (s == 0) { if (i > best_[0]) best_[0] = i; }
  else if (best_[1] < 0 || i < best_[1]) best_[1] = i;
}

// Retire le slot de sa file et met à jour bitmap / meilleur prix. Ne libère pas.
void CarnetPlat::delier(uint32_t sl, int s) {
  const Slot& x = slots_[sl];
  Niveau& n = niv_[s][x.niv];
  if (x.prev != NIL) slots_[x.prev].next = x.next; else n.tete = x.next;
  if (x.next != NIL) slots_[x.next].prev = x.prev; else n.queue = x.prev;
  n.total -= x.qte;
  total_[s] -= x.qte;
  if (n.tete == NIL) {
    bits_[s][x.niv >> 6] &= ~(1ULL << (x.niv & 63));
    if (best_[s] == x.niv) best_[s] = prochain(s, x.niv);
  }
}

// so = côté consommé. limite = indice de niveau limite (entier extrême pour un ordre au marché).
Qte CarnetPlat::apparier(int so, Id taker, Cote ct, Qte qte, int limite) {
  while (qte > 0) {
    const int b = best_[so];
    if (b < 0) break;
    if (so == 1 ? b > limite : b < limite) break;
    Niveau& n = niv_[so][b];
    while (qte > 0 && n.tete != NIL) {
      Slot& m = slots_[n.tete];
      const Qte x = std::min(qte, m.qte);
      trades_.push_back({m.id, taker, base_ + b, x, ct});
      m.qte -= x;
      n.total -= x;
      total_[so] -= x;
      qte -= x;
      if (m.qte == 0) {
        const uint32_t sl = n.tete;
        n.tete = m.next;
        if (n.tete == NIL) n.queue = NIL; else slots_[n.tete].prev = NIL;
        pos_[m.id] = NIL;
        liberer(sl);
        --nb_vivants_;
      }
    }
    if (n.tete == NIL) {
      bits_[so][b >> 6] &= ~(1ULL << (b & 63));
      best_[so] = prochain(so, b);
    }
  }
  return qte;
}

bool CarnetPlat::ajouter_limite(Id id, Cote cote, Prix prix, Qte qte) {
  if (qte <= 0 || id >= ID_MAX || !cfg_.valide(prix) || contient(id)) return false;
  if (id >= pos_.size()) pos_.resize(std::max<size_t>(id + 1, pos_.size() * 2), NIL);
  const int s = idx(cote), i = prix - base_;
  const Qte reste = apparier(1 - s, id, cote, qte, i);
  if (reste > 0) poser(id, s, i, reste);
  return true;
}

bool CarnetPlat::annuler(Id id) {
  if (!contient(id)) return false;
  const uint32_t sl = pos_[id];
  delier(sl, idx(slots_[sl].cote));
  pos_[id] = NIL;
  liberer(sl);
  --nb_vivants_;
  return true;
}

bool CarnetPlat::modifier(Id id, Prix np, Qte nq) {
  if (!contient(id) || nq <= 0) return false;
  const uint32_t sl = pos_[id];
  Slot& x = slots_[sl];
  const int s = idx(x.cote);
  if (np == base_ + x.niv) {
    if (nq == x.qte) return true;
    if (nq < x.qte) {  // baisse : on garde sa place dans la file
      const Qte delta = x.qte - nq;
      x.qte = nq;
      niv_[s][x.niv].total -= delta;
      total_[s] -= delta;
      return true;
    }
    const int i = x.niv;  // hausse : fin de file, sans appariement possible
    delier(sl, s);
    pos_[id] = NIL;
    liberer(sl);
    --nb_vivants_;
    poser(id, s, i, nq);
    return true;
  }
  if (!cfg_.valide(np)) return false;
  const Cote c = x.cote;
  annuler(id);
  return ajouter_limite(id, c, np, nq);
}

Qte CarnetPlat::ordre_marche(Id id, Cote cote, Qte qte) {
  if (qte <= 0) return 0;
  const int s = idx(cote);
  // Achat : on consomme les ventes (so=1), aucune borne haute ; Vente : borne basse absente.
  const Qte reste = apparier(1 - s, id, cote, qte, s == 0 ? INT_MAX : INT_MIN);
  return qte - reste;
}

int64_t CarnetPlat::qte_niveau(Cote c, Prix p) const {
  if (!cfg_.valide(p)) return 0;
  return niv_[idx(c)][p - base_].total;
}

int CarnetPlat::profondeur(Cote c, NiveauVue* out, int n) const {
  const int s = idx(c);
  int i = 0, b = best_[s];
  while (i < n && b >= 0) {
    out[i++] = {base_ + b, niv_[s][b].total};
    b = prochain(s, b);
  }
  return i;
}

}  // namespace marche
