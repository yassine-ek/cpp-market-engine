// Référence de comparaison : file bornée std::queue protégée par std::mutex.
// Même API (try_push / try_pop) et même capacité que FileSpsc, pour une charge identique.
#pragma once
#include <cstddef>
#include <mutex>
#include <queue>

namespace marche {

template <class T>
class FileMutex {
 public:
  explicit FileMutex(size_t capacite) : cap_(capacite) {}
  size_t capacite() const { return cap_; }

  bool try_push(const T& v) {
    std::lock_guard<std::mutex> g(m_);
    if (q_.size() >= cap_) return false;
    q_.push(v);
    return true;
  }
  bool try_pop(T& out) {
    std::lock_guard<std::mutex> g(m_);
    if (q_.empty()) return false;
    out = q_.front();
    q_.pop();
    return true;
  }

 private:
  const size_t cap_;
  std::mutex m_;
  std::queue<T> q_;
};

}  // namespace marche
