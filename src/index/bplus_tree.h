#pragma once
#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common/result.h"

namespace minidb {

// 메모리 기반 B+ 트리 (키 중복 불가).
//  - 모든 값은 리프에 저장되고, 리프는 next 포인터로 연결되어 범위 스캔이 가능하다.
//  - 내부 노드의 keys[i]는 children[i+1] 서브트리의 최솟값(분리 키)이다.
//    즉 키 k는 upper_bound(keys, k) 번째 자식으로 내려간다 (k == keys[i] 이면 오른쪽 자식).
//  - kMaxKeys: 노드 하나가 가질 수 있는 최대 키 수. 초과하면 분할한다.
// 스레드 안전하지 않다: Table 의 shared_mutex 가 보호한다 (읽기 = shared, 쓰기 = unique).
template <class K, class V, int kMaxKeys = 128>
class BPlusTree {
  static_assert(kMaxKeys >= 3, "kMaxKeys must be >= 3");

 public:
  BPlusTree() : root_(std::make_unique<Leaf>()) {}

  size_t size() const { return size_; }
  int height() const {
    int h = 1;
    for (const Node* n = root_.get(); !n->leaf; n = static_cast<const Internal*>(n)->children[0].get()) ++h;
    return h;
  }

  // 키가 이미 있으면 false (삽입하지 않음).
  bool insert(const K& key, const V& value) {
    std::optional<SplitResult> split;
    if (!insert_rec(root_.get(), key, value, &split)) return false;
    if (split) {  // 루트가 분할되면 트리 높이가 1 증가
      auto new_root = std::make_unique<Internal>();
      new_root->keys.push_back(std::move(split->key));
      new_root->children.push_back(std::move(root_));
      new_root->children.push_back(std::move(split->right));
      root_ = std::move(new_root);
    }
    ++size_;
    return true;
  }

  std::optional<V> find(const K& key) const {
    const Leaf* leaf = find_leaf(key);
    auto it = std::lower_bound(leaf->keys.begin(), leaf->keys.end(), key);
    if (it == leaf->keys.end() || key < *it) return std::nullopt;
    return leaf->vals[static_cast<size_t>(it - leaf->keys.begin())];
  }

  // lo <= key <= hi 인 항목을 키 오름차순으로 방문한다. fn(key, value)가 false 를 반환하면 중단.
  template <class Fn>
  void range(const K& lo, const K& hi, Fn&& fn) const {
    if (hi < lo) return;
    const Leaf* leaf = find_leaf(lo);
    size_t i = static_cast<size_t>(std::lower_bound(leaf->keys.begin(), leaf->keys.end(), lo) - leaf->keys.begin());
    while (leaf) {
      for (; i < leaf->keys.size(); ++i) {
        if (hi < leaf->keys[i]) return;
        if (!fn(leaf->keys[i], leaf->vals[i])) return;
      }
      leaf = leaf->next;
      i = 0;
    }
  }

  // 구조 불변식 검증 (테스트/진단용): 정렬, 분리 키 범위, 모든 리프 같은 깊이, 최소 점유율, 리프 연결, 크기.
  Status validate() const {
    int leaf_depth = -1;
    size_t counted = 0;
    Status st = validate_node(root_.get(), nullptr, nullptr, 1, &leaf_depth, &counted, true);
    if (!st.ok()) return st;
    if (counted != size_) return bad("leaf key count " + std::to_string(counted) + " != size " + std::to_string(size_));
    // 리프 연결 리스트가 전체 키를 오름차순으로 이어야 한다.
    const Node* n = root_.get();
    while (!n->leaf) n = static_cast<const Internal*>(n)->children[0].get();
    size_t chained = 0;
    const K* prev = nullptr;
    for (const Leaf* l = static_cast<const Leaf*>(n); l; l = l->next) {
      for (const K& k : l->keys) {
        if (prev && !(*prev < k)) return bad("leaf chain not strictly increasing");
        prev = &k;
        ++chained;
      }
    }
    if (chained != size_) return bad("leaf chain covers " + std::to_string(chained) + " keys, expected " + std::to_string(size_));
    return Status::success();
  }

 private:
  struct Node {
    explicit Node(bool is_leaf) : leaf(is_leaf) {}
    virtual ~Node() = default;
    bool leaf;
    std::vector<K> keys;
  };
  struct Leaf : Node {
    Leaf() : Node(true) {}
    std::vector<V> vals;
    Leaf* next = nullptr;
  };
  struct Internal : Node {
    Internal() : Node(false) {}
    std::vector<std::unique_ptr<Node>> children;
  };
  struct SplitResult {
    K key;
    std::unique_ptr<Node> right;
  };

  static Status bad(const std::string& m) { return Status::fail(ErrorCode::kInternal, "B+ tree invariant violated: " + m); }

  static size_t child_index(const Internal* n, const K& key) {
    return static_cast<size_t>(std::upper_bound(n->keys.begin(), n->keys.end(), key) - n->keys.begin());
  }

  const Leaf* find_leaf(const K& key) const {
    const Node* n = root_.get();
    while (!n->leaf) {
      const Internal* in = static_cast<const Internal*>(n);
      n = in->children[child_index(in, key)].get();
    }
    return static_cast<const Leaf*>(n);
  }

  // 재귀 삽입. 자식이 분할되면 *split 에 (분리 키, 새 오른쪽 노드)를 돌려주고 부모가 흡수한다.
  bool insert_rec(Node* node, const K& key, const V& value, std::optional<SplitResult>* split) {
    if (node->leaf) {
      Leaf* leaf = static_cast<Leaf*>(node);
      auto it = std::lower_bound(leaf->keys.begin(), leaf->keys.end(), key);
      if (it != leaf->keys.end() && !(key < *it)) return false;  // 중복
      size_t pos = static_cast<size_t>(it - leaf->keys.begin());
      leaf->keys.insert(leaf->keys.begin() + static_cast<std::ptrdiff_t>(pos), key);
      leaf->vals.insert(leaf->vals.begin() + static_cast<std::ptrdiff_t>(pos), value);
      if (leaf->keys.size() > static_cast<size_t>(kMaxKeys)) {
        // 리프 분할: 오른쪽 절반을 새 리프로 옮기고, 그 첫 키를 부모에 "복사"해 올린다.
        size_t mid = (leaf->keys.size() + 1) / 2;
        auto right = std::make_unique<Leaf>();
        right->keys.assign(std::make_move_iterator(leaf->keys.begin() + static_cast<std::ptrdiff_t>(mid)),
                           std::make_move_iterator(leaf->keys.end()));
        right->vals.assign(std::make_move_iterator(leaf->vals.begin() + static_cast<std::ptrdiff_t>(mid)),
                           std::make_move_iterator(leaf->vals.end()));
        leaf->keys.resize(mid);
        leaf->vals.resize(mid);
        right->next = leaf->next;
        leaf->next = right.get();
        K sep = right->keys.front();
        *split = SplitResult{std::move(sep), std::move(right)};
      }
      return true;
    }
    Internal* in = static_cast<Internal*>(node);
    size_t idx = child_index(in, key);
    std::optional<SplitResult> child_split;
    if (!insert_rec(in->children[idx].get(), key, value, &child_split)) return false;
    if (child_split) {
      in->keys.insert(in->keys.begin() + static_cast<std::ptrdiff_t>(idx), std::move(child_split->key));
      in->children.insert(in->children.begin() + static_cast<std::ptrdiff_t>(idx + 1), std::move(child_split->right));
      if (in->keys.size() > static_cast<size_t>(kMaxKeys)) {
        // 내부 노드 분할: 가운데 키는 부모로 "이동"(올라가고 양쪽에는 남지 않음)한다.
        size_t mid = in->keys.size() / 2;
        auto right = std::make_unique<Internal>();
        K up = std::move(in->keys[mid]);
        right->keys.assign(std::make_move_iterator(in->keys.begin() + static_cast<std::ptrdiff_t>(mid + 1)),
                           std::make_move_iterator(in->keys.end()));
        right->children.assign(std::make_move_iterator(in->children.begin() + static_cast<std::ptrdiff_t>(mid + 1)),
                               std::make_move_iterator(in->children.end()));
        in->keys.resize(mid);
        in->children.resize(mid + 1);
        *split = SplitResult{std::move(up), std::move(right)};
      }
    }
    return true;
  }

  // lo <= 모든 키 < hi (nullptr 이면 무제한).
  Status validate_node(const Node* node, const K* lo, const K* hi, int depth, int* leaf_depth, size_t* counted,
                       bool is_root) const {
    for (size_t i = 0; i < node->keys.size(); ++i) {
      if (i > 0 && !(node->keys[i - 1] < node->keys[i])) return bad("keys not strictly increasing");
      if (lo && node->keys[i] < *lo) return bad("key below lower bound");
      if (hi && !(node->keys[i] < *hi)) return bad("key above upper bound");
    }
    if (node->keys.size() > static_cast<size_t>(kMaxKeys)) return bad("node overflow");
    if (node->leaf) {
      const Leaf* l = static_cast<const Leaf*>(node);
      if (l->vals.size() != l->keys.size()) return bad("leaf keys/vals size mismatch");
      if (!is_root && l->keys.size() < static_cast<size_t>(kMaxKeys / 2)) return bad("leaf underflow");
      if (*leaf_depth == -1) *leaf_depth = depth;
      else if (*leaf_depth != depth) return bad("leaves at different depths");
      *counted += l->keys.size();
      return Status::success();
    }
    const Internal* in = static_cast<const Internal*>(node);
    if (in->children.size() != in->keys.size() + 1) return bad("internal children/keys mismatch");
    size_t min_keys = is_root ? 1 : static_cast<size_t>((kMaxKeys - 1) / 2);
    if (in->keys.size() < min_keys) return bad("internal underflow");
    for (size_t i = 0; i < in->children.size(); ++i) {
      const K* clo = i == 0 ? lo : &in->keys[i - 1];
      const K* chi = i == in->keys.size() ? hi : &in->keys[i];
      Status st = validate_node(in->children[i].get(), clo, chi, depth + 1, leaf_depth, counted, false);
      if (!st.ok()) return st;
    }
    return Status::success();
  }

  std::unique_ptr<Node> root_;
  size_t size_ = 0;
};

}  // namespace minidb
