// Copyright 2017 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_COMPILER_PERSISTENT_H_
#define V8_COMPILER_PERSISTENT_H_

#include <array>
#include <bitset>
#include <tuple>

#include "src/base/functional.h"
#include "src/zone/zone-containers.h"

namespace v8 {
namespace internal {
namespace compiler {

// PersistentMap is a persistent map datastructure based on hash trees (a binary
// tree using the bits of a hash value as addresses). The map is a conceptually
// infinite: All keys are initially mapped to a default value, values are
// deleted by overwriting them with the default value. The iterators produce
// exactly the keys that are not the default value. The hash values should have
// high variance in their high bits, so dense integers are a bad choice.
// Complexity:
// - Copy and assignment: O(1)
// - access: O(log n)
// - update: O(log n) time and space
// - iteration: amortized O(1) per step
// - Zip: O(n)
// - equality check: O(n)
// TODO (tebbi): Cache map transitions to avoid re-allocation of the same map. id:792 gh:800
// TODO (tebbi): Implement an O(1) equality check based on hash consing or id:652 gh:653
//              something similar.
template <class Key, class Value, class Hasher = base::hash<Key>>
class PersistentMap {
 public:
  using key_type = Key;
  using mapped_type = Value;
  using value_type = std::pair<Key, Value>;

 private:
  static constexpr size_t kHashBits = 32;
  enum Bit : int { kLeft = 0, kRight = 1 };

  // Access hash bits starting from the high bits and compare them according to
  // their unsigned value. This way, the order in the hash tree is compatible
  // with numeric hash comparisons.
  class HashValue;

  struct KeyValue : std::pair<Key, Value> {
    const Key& key() const { return this->first; }
    const Value& value() const { return this->second; }
    using std::pair<Key, Value>::pair;
  };

  struct FocusedTree;

 public:
  // Depth of the last added element. This is a cheap estimate for the size of
  // the hash tree.
  size_t last_depth() const {
    if (tree_) {
      return tree_->length;
    } else {
      return 0;
    }
  }

  const Value& Get(const Key& key) const {
    HashValue key_hash = HashValue(Hasher()(key));
    const FocusedTree* tree = FindHash(key_hash);
    return GetFocusedValue(tree, key);
  }

  // Add or overwrite an existing key-value pair.
  PersistentMap Add(Key key, Value value) const;
  void Set(Key key, Value value) { *this = Add(key, value); }

  bool operator==(const PersistentMap& other) const {
    if (tree_ == other.tree_) return true;
    if (def_value_ != other.def_value_) return false;
    for (const std::tuple<Key, Value, Value>& triple : Zip(other)) {
      if (std::get<1>(triple) != std::get<2>(triple)) return false;
    }
    return true;
  }

  bool operator!=(const PersistentMap& other) const {
    return !(*this == other);
  }

  // The iterator produces key-value pairs in the lexicographical order of
  // hash value and key. It produces exactly the key-value pairs where the value
  // is not the default value.
  class iterator;

  iterator begin() const {
    if (!tree_) return end();
    return iterator::begin(tree_, def_value_);
  }
  iterator end() const { return iterator::end(def_value_); }

  // Iterator to traverse two maps in lockstep, producing matching value pairs
  // for each key where at least one value is different from the respective
  // default.
  class double_iterator;

  // An iterable to iterate over the two maps in lockstep.
  struct ZipIterable {
    PersistentMap a;
    PersistentMap b;
    double_iterator begin() { return double_iterator(a.begin(), b.begin()); }
    double_iterator end() { return double_iterator(a.end(), b.end()); }
  };

  ZipIterable Zip(const PersistentMap& other) const { return {*this, other}; }

  explicit PersistentMap(Zone* zone, Value def_value = Value())
      : PersistentMap(nullptr, zone, def_value) {}

 private:
  // Find the {FocusedTree} that contains a key-value pair with key hash {hash}.
  const FocusedTree* FindHash(HashValue hash) const;

  // Find the {FocusedTree} that contains a key-value pair with key hash {hash}.
  // Output the path to this {FocusedTree} and its length. If no such
  // {FocusedTree} exists, return {nullptr} and output the path to the last node
  // with a matching hash prefix. Note that {length} is the length of the found
  // path and may be less than the length of the found {FocusedTree}.
  const FocusedTree* FindHash(HashValue hash,
                              std::array<const FocusedTree*, kHashBits>* path,
                              int* length) const;

  // Load value from the leaf node on the focused path of {tree}.
  const Value& GetFocusedValue(const FocusedTree* tree, const Key& key) const;

  // Return the {FocusedTree} representing the left (bit==kLeft) or right
  // (bit==kRight) child of the node on the path of {tree} at tree level
  // {level}.
  static const FocusedTree* GetChild(const FocusedTree* tree, int level,
                                     Bit bit);

  // Find the leftmost path in the tree, starting at the node at tree level
  // {level} on the path of {start}. Output the level of the leaf to {level} and
  // the path to {path}.
  static const FocusedTree* FindLeftmost(
      const FocusedTree* start, int* level,
      std::array<const FocusedTree*, kHashBits>* path);

  PersistentMap(const FocusedTree* tree, Zone* zone, Value def_value)
      : tree_(tree), def_value_(def_value), zone_(zone) {}

  const FocusedTree* tree_;
  Value def_value_;
  Zone* zone_;
};

// This structure represents a hash tree with one focused path to a specific
// leaf. For the focused leaf, it stores key, value and key hash. The path is
// defined by the hash bits of the focused leaf. In a traditional tree
// datastructure, the nodes of a path form a linked list with the values being
// the pointers outside of this path. Instead of storing all of these nodes,
// we store an array of the pointers pointing outside of the path. This is
// similar to the stack used when doing DFS traversal of a tree. The hash of
// the leaf is used to know if the pointers point to the left or the
// right of the path. As there is no explicit representation of a tree node,
// this structure also represents all the nodes on its path. The intended node
// depends on the tree depth, which is always clear from the referencing
// context. So the pointer to a {FocusedTree} stored in the
// {PersistentMap.tree_} always references the root, while a pointer from a
// focused node of another {FocusedTree} always references to one tree level
// lower than before.
template <class Key, class Value, class Hasher>
struct PersistentMap<Key, Value, Hasher>::FocusedTree {
  KeyValue key_value;
  // The depth of the focused path, that is, the number of pointers stored in
  // this structure.
  int8_t length;
  HashValue key_hash;
  // Out-of-line storage for hash collisions.
  const ZoneMap<Key, Value>* more;
  using more_iterator = typename ZoneMap<Key, Value>::const_iterator;
  // {path_array} has to be the last member: To store an array inline, we
  // over-allocate memory for this structure and access memory beyond
  // {path_array}. This corresponds to a flexible array member as defined in
  // C99.
  const FocusedTree* path_array[1];
  const FocusedTree*& path(int i) {
    DCHECK(i < length);
    return reinterpret_cast<const FocusedTree**>(
        reinterpret_cast<byte*>(this) + offsetof(FocusedTree, path_array))[i];
  }
  const FocusedTree* path(int i) const {
    DCHECK(i < length);
    return reinterpret_cast<const FocusedTree* const*>(
        reinterpret_cast<const byte*>(this) +
        offsetof(FocusedTree, path_array))[i];
  }
};

template <class Key, class Value, class Hasher>
class PersistentMap<Key, Value, Hasher>::HashValue {
 public:
  explicit HashValue(size_t hash) : bits_(hash) {}
  explicit HashValue(std::bitset<kHashBits> hash) : bits_(hash) {}

  Bit operator[](int pos) const {
    return bits_[kHashBits - pos - 1] ? kRight : kLeft;
  }

  bool operator<(HashValue other) const {
    static_assert(sizeof(*this) <= sizeof(unsigned long), "");  // NOLINT
    return bits_.to_ulong() < other.bits_.to_ulong();
  }
  bool operator==(HashValue other) const { return bits_ == other.bits_; }
  bool operator!=(HashValue other) const { return bits_ != other.bits_; }
  HashValue operator^(HashValue other) const {
    return HashValue(bits_ ^ other.bits_);
  }

 private:
  std::bitset<kHashBits> bits_;
};

template <class Key, class Value, class Hasher>
class PersistentMap<Key, Value, Hasher>::iterator {
 public:
  const value_type operator*() const {
    if (current_->more) {
      return *more_iter_;
    } else {
      return current_->key_value;
    }
  }

  iterator& operator++() {
    do {
      if (!current_) {
        // Iterator is past the end.
        return *this;
      }
      if (current_->more) {
        DCHECK(more_iter_ != current_->more->end());
        ++more_iter_;
        if (more_iter_ != current_->more->end()) return *this;
      }
      if (level_ == 0) {
        *this = end(def_value_);
        return *this;
      }
      --level_;
      while (current_->key_hash[level_] == kRight || path_[level_] == nullptr) {
        if (level_ == 0) {
          *this = end(def_value_);
          return *this;
        }
        --level_;
      }
      const FocusedTree* first_right_alternative = path_[level_];
      level_++;
      current_ = FindLeftmost(first_right_alternative, &level_, &path_);
      if (current_->more) {
        more_iter_ = current_->more->begin();
      }
    } while ((**this).second == def_value());
    return *this;
  }

  bool operator==(const iterator& other) const {
    if (is_end()) return other.is_end();
    if (other.is_end()) return false;
    if (current_->key_hash != other.current_->key_hash) {
      return false;
    } else {
      return (**this).first == (*other).first;
    }
  }
  bool operator!=(const iterator& other) const { return !(*this == other); }

  bool operator<(const iterator& other) const {
    if (is_end()) return false;
    if (other.is_end()) return true;
    if (current_->key_hash < other.current_->key_hash) {
      return true;
    } else if (current_->key_hash == other.current_->key_hash) {
      return (**this).first < (*other).first;
    } else {
      return false;
    }
  }

  bool is_end() const { return current_ == nullptr; }

  const Value& def_value() { return def_value_; }

  static iterator begin(const FocusedTree* tree, Value def_value) {
    iterator i(def_value);
    i.current_ = FindLeftmost(tree, &i.level_, &i.path_);
    if (i.current_->more) {
      i.more_iter_ = i.current_->more->begin();
    }
    return i;
  }

  static iterator end(Value def_value) { return iterator(def_value); }

 private:
  int level_;
  typename FocusedTree::more_iterator more_iter_;
  const FocusedTree* current_;
  std::array<const FocusedTree*, kHashBits> path_;
  Value def_value_;

  explicit iterator(Value def_value)
      : level_(0), current_(nullptr), def_value_(def_value) {}
};

template <class Key, class Value, class Hasher>
class PersistentMap<Key, Value, Hasher>::double_iterator {
 public:
  std::tuple<Key, Value, Value> operator*() {
    if (first_current_) {
      auto pair = *first_;
      return std::make_tuple(
          pair.first, pair.second,
          second_current_ ? (*second_).second : second_.def_value());
    } else {
      DCHECK(second_current_);
      auto pair = *second_;
      return std::make_tuple(pair.first, first_.def_value(), pair.second);
    }
  }

  double_iterator& operator++() {
    if (first_current_) ++first_;
    if (second_current_) ++second_;
    return *this = double_iterator(first_, second_);
  }

  double_iterator(iterator first, iterator second)
      : first_(first), second_(second) {
    if (first_ == second_) {
      first_current_ = second_current_ = true;
    } else if (first_ < second_) {
      first_current_ = true;
      second_current_ = false;
    } else {
      first_current_ = false;
      second_current_ = true;
    }
  }

  bool operator!=(const double_iterator& other) {
    return first_ != other.first_ || second_ != other.second_;
  }

  bool is_end() const { return first_.is_end() && second_.is_end(); }

 private:
  iterator first_;
  iterator second_;
  bool first_current_;
  bool second_current_;
};

template <class Key, class Value, class Hasher>
PersistentMap<Key, Value, Hasher> PersistentMap<Key, Value, Hasher>::Add(
    Key key, Value value) const {
  HashValue key_hash = HashValue(Hasher()(key));
  std::array<const FocusedTree*, kHashBits> path;
  int length = 0;
  const FocusedTree* old = FindHash(key_hash, &path, &length);
  ZoneMap<Key, Value>* more = nullptr;
  if (GetFocusedValue(old, key) == value) return *this;
  if (old && !(old->more == nullptr && old->key_value.key() == key)) {
    more = new (zone_->New(sizeof(*more))) ZoneMap<Key, Value>(zone_);
    if (old->more) {
      *more = *old->more;
    } else {
      (*more)[old->key_value.key()] = old->key_value.value();
    }
    (*more)[key] = value;
  }
  FocusedTree* tree =
      new (zone_->New(sizeof(FocusedTree) +
                      std::max(0, length - 1) * sizeof(const FocusedTree*)))
          FocusedTree{KeyValue(std::move(key), std::move(value)),
                      static_cast<int8_t>(length),
                      key_hash,
                      more,
                      {}};
  for (int i = 0; i < length; ++i) {
    tree->path(i) = path[i];
  }
  return PersistentMap(tree, zone_, def_value_);
}

template <class Key, class Value, class Hasher>
const typename PersistentMap<Key, Value, Hasher>::FocusedTree*
PersistentMap<Key, Value, Hasher>::FindHash(HashValue hash) const {
  const FocusedTree* tree = tree_;
  int level = 0;
  while (tree && hash != tree->key_hash) {
    while ((hash ^ tree->key_hash)[level] == 0) {
      ++level;
    }
    tree = level < tree->length ? tree->path(level) : nullptr;
    ++level;
  }
  return tree;
}

template <class Key, class Value, class Hasher>
const typename PersistentMap<Key, Value, Hasher>::FocusedTree*
PersistentMap<Key, Value, Hasher>::FindHash(
    HashValue hash, std::array<const FocusedTree*, kHashBits>* path,
    int* length) const {
  const FocusedTree* tree = tree_;
  int level = 0;
  while (tree && hash != tree->key_hash) {
    int map_length = tree->length;
    while ((hash ^ tree->key_hash)[level] == 0) {
      (*path)[level] = level < map_length ? tree->path(level) : nullptr;
      ++level;
    }
    (*path)[level] = tree;
    tree = level < tree->length ? tree->path(level) : nullptr;
    ++level;
  }
  if (tree) {
    while (level < tree->length) {
      (*path)[level] = tree->path(level);
      ++level;
    }
  }
  *length = level;
  return tree;
}

template <class Key, class Value, class Hasher>
const Value& PersistentMap<Key, Value, Hasher>::GetFocusedValue(
    const FocusedTree* tree, const Key& key) const {
  if (!tree) {
    return def_value_;
  }
  if (tree->more) {
    auto it = tree->more->find(key);
    if (it == tree->more->end())
      return def_value_;
    else
      return it->second;
  } else {
    if (key == tree->key_value.key()) {
      return tree->key_value.value();
    } else {
      return def_value_;
    }
  }
}

template <class Key, class Value, class Hasher>
const typename PersistentMap<Key, Value, Hasher>::FocusedTree*
PersistentMap<Key, Value, Hasher>::GetChild(const FocusedTree* tree, int level,
                                            Bit bit) {
  if (tree->key_hash[level] == bit) {
    return tree;
  } else if (level < tree->length) {
    return tree->path(level);
  } else {
    return nullptr;
  }
}

template <class Key, class Value, class Hasher>
const typename PersistentMap<Key, Value, Hasher>::FocusedTree*
PersistentMap<Key, Value, Hasher>::FindLeftmost(
    const FocusedTree* start, int* level,
    std::array<const FocusedTree*, kHashBits>* path) {
  const FocusedTree* current = start;
  while (*level < current->length) {
    if (const FocusedTree* child = GetChild(current, *level, kLeft)) {
      (*path)[*level] = GetChild(current, *level, kRight);
      current = child;
      ++*level;
    } else if (const FocusedTree* child = GetChild(current, *level, kRight)) {
      (*path)[*level] = GetChild(current, *level, kLeft);
      current = child;
      ++*level;
    } else {
      UNREACHABLE();
    }
  }
  return current;
}

template <class Key, class Value, class Hasher>
std::ostream& operator<<(std::ostream& os,
                         const PersistentMap<Key, Value, Hasher>& map) {
  os << "{";
  bool first = true;
  for (auto pair : map) {
    if (!first) os << ", ";
    first = false;
    os << pair.first << ": " << pair.second;
  }
  return os << "}";
}

}  // namespace compiler
}  // namespace internal
}  // namespace v8

#endif  // V8_COMPILER_PERSISTENT_MAP_H_
