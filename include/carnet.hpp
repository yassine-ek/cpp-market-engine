// Carnet d'ordres limite, priorité prix-temps (FIFO par niveau de prix).
//
// Deux implémentations à l'API identique, volontairement SANS héritage virtuel :
// le benchmark ne doit pas payer d'appel indirect, et les tests/bench les
// manipulent par template.
//   (a) CarnetMap  : std::map<prix, std::list<ordre>> + unordered_map id->iterateur
//   (b) CarnetPlat : tableau plat indexé par tick + pool d'ordres + index id->slot
#pragma once
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <unordered_map>
#include <vector>

namespace marche {

using Prix = int32_t;  // ticks entiers : un double rendrait les égalités de niveau fragiles
using Qte = int32_t;
using Id = uint32_t;
constexpr Prix SANS_PRIX = 0;  // renvoyé par meilleur() quand un côté est vide

enum class Cote : uint8_t { Achat = 0, Vente = 1 };
inline Cote oppose(Cote c) { return c == Cote::Achat ? Cote::Vente : Cote::Achat; }

struct Trade {
  Id maker;        // ordre au repos
  Id taker;        // ordre entrant
  Prix prix;       // toujours le prix du maker
  Qte qte;
  Cote cote_taker;
  bool operator==(const Trade&) const = default;
};

struct NiveauVue {
  Prix prix;
  int64_t qte;
};

// Les deux carnets partagent la même fenêtre de prix : le plat en a besoin,
// et l'imposer au map garantit un comportement strictement identique
// (donc un test différentiel sans exception).
struct Config {
  Prix prix_ref = 10000;
  Prix demi_largeur = 2048;      // prix valides : [prix_ref - demi, prix_ref + demi]
  uint32_t ordres_max = 1 << 20; // réserve initiale : pas d'allocation en régime établi
  uint32_t ids_max = 1 << 20;    // idem pour la table id -> position
  bool valide(Prix p) const { return p >= prix_ref - demi_largeur && p <= prix_ref + demi_largeur; }
};

// Garde-fou : le plat indexe un vecteur par id, un id géant ferait exploser la mémoire.
constexpr Id ID_MAX = 1u << 28;

class JournalTrades {
 public:
  const std::vector<Trade>& trades() const { return trades_; }
  void vider_trades() { trades_.clear(); }  // la capacité est conservée

 protected:
  JournalTrades() { trades_.reserve(4096); }
  std::vector<Trade> trades_;
};

// ------------------------------------------------------------------ (a) map
class CarnetMap : public JournalTrades {
 public:
  explicit CarnetMap(const Config& cfg = {});

  // true si l'ordre est accepté (le reliquat éventuel repose au carnet).
  bool ajouter_limite(Id id, Cote cote, Prix prix, Qte qte);
  bool annuler(Id id);
  // Quantité restante := nouvelle_qte. Même prix : baisse = priorité gardée,
  // hausse = fin de file. Prix différent : annulation + réinsertion (peut croiser).
  bool modifier(Id id, Prix nouveau_prix, Qte nouvelle_qte);
  // Renvoie la quantité exécutée ; le reliquat est abandonné (IOC).
  Qte ordre_marche(Id id, Cote cote, Qte qte);
  void vider();

  bool contient(Id id) const { return index_.count(id) != 0; }
  Qte qte_ordre(Id id) const;  // 0 si absent
  Prix meilleur(Cote c) const;
  int64_t qte_totale(Cote c) const { return total_[static_cast<int>(c)]; }
  int64_t qte_niveau(Cote c, Prix p) const;
  size_t nb_ordres() const { return index_.size(); }
  int profondeur(Cote c, NiveauVue* out, int n) const;

 private:
  struct Ordre { Id id; Qte qte; };
  using File = std::list<Ordre>;
  struct Niveau { File file; int64_t total = 0; };
  struct Loc { Cote cote; Prix prix; File::iterator it; };

  template <class M, class Croise>
  Qte apparier(M& opp, int64_t& total_opp, Id taker, Cote ct, Qte qte, Croise croise);
  void retirer(const Loc& l);
  void poser(Id id, Cote c, Prix p, Qte q);

  Config cfg_;
  std::map<Prix, Niveau, std::greater<Prix>> achats_;  // meilleur = begin()
  std::map<Prix, Niveau> ventes_;
  std::unordered_map<Id, Loc> index_;
  int64_t total_[2] = {0, 0};
};

// ----------------------------------------------------------------- (b) plat
class CarnetPlat : public JournalTrades {
 public:
  explicit CarnetPlat(const Config& cfg = {});

  bool ajouter_limite(Id id, Cote cote, Prix prix, Qte qte);
  bool annuler(Id id);
  bool modifier(Id id, Prix nouveau_prix, Qte nouvelle_qte);
  Qte ordre_marche(Id id, Cote cote, Qte qte);
  void vider();

  bool contient(Id id) const { return id < pos_.size() && pos_[id] != NIL; }
  Qte qte_ordre(Id id) const { return contient(id) ? slots_[pos_[id]].qte : 0; }
  Prix meilleur(Cote c) const {
    const int b = best_[static_cast<int>(c)];
    return b < 0 ? SANS_PRIX : base_ + b;
  }
  int64_t qte_totale(Cote c) const { return total_[static_cast<int>(c)]; }
  int64_t qte_niveau(Cote c, Prix p) const;
  size_t nb_ordres() const { return nb_vivants_; }
  int profondeur(Cote c, NiveauVue* out, int n) const;

 private:
  static constexpr uint32_t NIL = 0xFFFFFFFFu;
  // File FIFO par niveau = liste doublement chaînée par INDICES dans un pool
  // contigu (pas de pointeurs, pas de new/delete : slots recyclés via une free-list).
  struct Slot { Id id; Qte qte; uint32_t prev, next; int32_t niv; Cote cote; };  // 24 o
  struct Niveau { uint32_t tete = NIL, queue = NIL; int64_t total = 0; };  // 16 o

  Qte apparier(int so, Id taker, Cote ct, Qte qte, int limite);
  void poser(Id id, int s, int idx, Qte q);
  void delier(uint32_t slot, int s);
  int prochain(int s, int idx) const;  // niveau non vide suivant, en s'éloignant du touch
  uint32_t allouer();
  void liberer(uint32_t s) { slots_[s].next = libre_; libre_ = s; }

  Config cfg_;
  Prix base_;                 // prix du niveau d'indice 0
  int nb_niv_;                // multiple de 64 (bitmap)
  std::vector<Niveau> niv_[2];
  std::vector<uint64_t> bits_[2];  // bit i = niveau i non vide : trouver le meilleur suivant sans balayer
  int best_[2] = {-1, -1};         // indice du meilleur niveau, -1 si vide
  int64_t total_[2] = {0, 0};
  std::vector<Slot> slots_;
  uint32_t libre_ = NIL;
  std::vector<uint32_t> pos_;  // id -> slot
  size_t nb_vivants_ = 0;
};

}  // namespace marche
