// File SPSC (un producteur, un consommateur) lock-free, à capacité puissance de 2.
//
// Principe : deux compteurs monotones 64 bits, jamais remis à zéro.
//   - 'tail_' n'est écrit que par le producteur (nombre d'éléments publiés),
//   - 'tete_' n'est écrit que par le consommateur (nombre d'éléments consommés).
// L'élément k vit dans la case k & masque ; la file est vide si tete == tail, pleine si
// tail - tete == capacité. Les compteurs ne bouclent pas en pratique (2^64), donc pas de
// case sacrifiée pour distinguer plein et vide, et le wrap-around se réduit à un « & masque ».
//
// Pourquoi acquire/release suffit : chaque compteur n'a qu'UN écrivain, il n'y a donc aucune
// course d'écriture à arbitrer, seulement un ordre de visibilité à garantir :
//   - le producteur écrit la case PUIS publie tail_ en release ; le consommateur lit tail_ en
//     acquire : s'il voit le nouveau tail_, il voit aussi le contenu de la case ;
//   - symétriquement, le consommateur lit la case PUIS publie tete_ en release ; le
//     producteur lit tete_ en acquire avant de réutiliser la case (pas d'écrasement d'une
//     case encore en lecture).
// Pas besoin de seq_cst : aucun raisonnement ne dépend d'un ordre total entre les deux compteurs.
// Les lectures de son PROPRE compteur sont relaxed (personne d'autre ne l'écrit).
//
// Faux partage : les deux compteurs, écrits par des cœurs différents, sont chacun seuls sur
// leur ligne de cache de 64 o (alignas). Sinon chaque écriture invaliderait la ligne chez l'autre.
//
// Cache de l'index opposé : le producteur garde une copie 'tete_vue_' de tete_ et ne relit le
// vrai (une ligne de cache qui voyage entre cœurs) que lorsque la copie dit « plein ». Idem
// côté consommateur avec 'tail_vue_'. En régime fluide on traite des rafales d'éléments pour
// un seul transfert de ligne. La copie est toujours en retard, donc prudente : jamais fausse
// dans le sens dangereux (elle peut dire « plein » à tort, jamais « libre » à tort).
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <memory>

namespace marche {

constexpr size_t LIGNE_CACHE = 64;  // std::hardware_destructive_interference_size donne un warning sous GCC

template <class T>
class FileSpsc {
 public:
  // capacite doit être une puissance de 2 (>= 2) ; sinon std::abort via le test dans le constructeur.
  explicit FileSpsc(size_t capacite)
      : masque_(capacite - 1), cases_(new T[capacite]) {
    if (capacite < 2 || (capacite & (capacite - 1)) != 0) std::abort();
  }
  FileSpsc(const FileSpsc&) = delete;
  FileSpsc& operator=(const FileSpsc&) = delete;

  size_t capacite() const { return masque_ + 1; }

  // Producteur uniquement. false si la file est pleine.
  bool try_push(const T& v) {
    const uint64_t t = tail_.load(std::memory_order_relaxed);
    if (t - tete_vue_ > masque_) {  // plein d'après la copie : relire le vrai index
      tete_vue_ = tete_.load(std::memory_order_acquire);
      if (t - tete_vue_ > masque_) return false;
    }
    cases_[t & masque_] = v;
    tail_.store(t + 1, std::memory_order_release);
    return true;
  }

  // Consommateur uniquement. false si la file est vide.
  bool try_pop(T& out) {
    const uint64_t h = tete_.load(std::memory_order_relaxed);
    if (h == tail_vue_) {  // vide d'après la copie : relire le vrai index
      tail_vue_ = tail_.load(std::memory_order_acquire);
      if (h == tail_vue_) return false;
    }
    out = cases_[h & masque_];
    tete_.store(h + 1, std::memory_order_release);
    return true;
  }

 private:
  const size_t masque_;
  std::unique_ptr<T[]> cases_;
  // Ligne du producteur : son compteur et sa copie de l'index du consommateur.
  alignas(LIGNE_CACHE) std::atomic<uint64_t> tail_{0};
  uint64_t tete_vue_ = 0;
  // Ligne du consommateur.
  alignas(LIGNE_CACHE) std::atomic<uint64_t> tete_{0};
  uint64_t tail_vue_ = 0;
  // Remplissage : empêche un objet voisin de partager la ligne du consommateur.
  alignas(LIGNE_CACHE) char fin_[1] = {0};
};

}  // namespace marche
