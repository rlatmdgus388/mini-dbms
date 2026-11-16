#include <algorithm>
#include <map>
#include <numeric>
#include <random>

#include "index/bplus_tree.h"
#include "test_util.h"

using namespace minidb;

namespace {

// 작은 차수(3,4,5)에서는 분할이 자주 일어나 구조 버그가 빨리 드러난다.
template <int M>
void run_order_checks() {
  const int n = 2000;
  std::vector<int64_t> keys(n);
  std::iota(keys.begin(), keys.end(), 1);

  auto check_with = [&](const std::vector<int64_t>& order) {
    BPlusTree<int64_t, int64_t, M> t;
    std::map<int64_t, int64_t> ref;
    for (int64_t k : order) {
      CHECK(t.insert(k, k * 10));
      ref[k] = k * 10;
    }
    CHECK_EQ(t.size(), ref.size());
    Status st = t.validate();
    CHECK(st.ok());
    if (!st.ok()) std::fprintf(stderr, "    %s\n", st.message().c_str());
    for (int64_t k : order) {
      auto v = t.find(k);
      CHECK(v.has_value() && *v == k * 10);
    }
    CHECK(!t.find(0).has_value());
    CHECK(!t.find(n + 1).has_value());
    CHECK(!t.insert(order[0], 1));  // 중복 거부
    CHECK_EQ(t.size(), ref.size());
    // 전체 순회가 정렬되어 있어야 한다
    std::vector<int64_t> all;
    t.range(INT64_MIN, INT64_MAX, [&](int64_t k, int64_t) { all.push_back(k); return true; });
    CHECK(all.size() == ref.size() && std::is_sorted(all.begin(), all.end()));
    // std::map 대조: 임의 범위
    std::mt19937_64 rng(42);
    for (int i = 0; i < 200; ++i) {
      int64_t a = static_cast<int64_t>(rng() % (n + 20)) - 10, b = static_cast<int64_t>(rng() % (n + 20)) - 10;
      std::vector<int64_t> got, want;
      t.range(a, b, [&](int64_t k, int64_t) { got.push_back(k); return true; });
      for (auto it = ref.lower_bound(a); it != ref.end() && it->first <= b; ++it) want.push_back(it->first);
      CHECK(got == want);
    }
  };

  check_with(keys);  // 오름차순
  std::vector<int64_t> desc(keys.rbegin(), keys.rend());
  check_with(desc);  // 내림차순
  std::mt19937_64 rng(7);
  std::vector<int64_t> shuffled = keys;
  std::shuffle(shuffled.begin(), shuffled.end(), rng);
  check_with(shuffled);  // 랜덤
}

}  // namespace

TEST(bplus_order_3) { run_order_checks<3>(); }
TEST(bplus_order_4) { run_order_checks<4>(); }
TEST(bplus_order_5) { run_order_checks<5>(); }
TEST(bplus_order_64) { run_order_checks<64>(); }

TEST(bplus_empty_tree) {
  BPlusTree<int64_t, int64_t, 4> t;
  CHECK_EQ(t.size(), size_t{0});
  CHECK(t.validate().ok());
  CHECK(!t.find(1).has_value());
  int calls = 0;
  t.range(INT64_MIN, INT64_MAX, [&](int64_t, int64_t) { ++calls; return true; });
  CHECK_EQ(calls, 0);
  CHECK_EQ(t.height(), 1);
}

TEST(bplus_range_edge_cases) {
  BPlusTree<int64_t, int64_t, 4> t;
  for (int64_t k = 10; k <= 100; k += 10) t.insert(k, k);
  auto collect = [&](int64_t lo, int64_t hi, size_t stop_after = SIZE_MAX) {
    std::vector<int64_t> v;
    t.range(lo, hi, [&](int64_t k, int64_t) { v.push_back(k); return v.size() < stop_after; });
    return v;
  };
  CHECK(collect(30, 30) == std::vector<int64_t>({30}));      // 점 조회
  CHECK(collect(31, 39).empty());                            // 키 사이
  CHECK(collect(50, 40).empty());                            // lo > hi
  CHECK(collect(-5, 15) == std::vector<int64_t>({10}));      // 하한 밖
  CHECK(collect(95, 1000) == std::vector<int64_t>({100}));   // 상한 밖
  CHECK_EQ(collect(INT64_MIN, INT64_MAX).size(), size_t{10});
  CHECK_EQ(collect(INT64_MIN, INT64_MAX, 3).size(), size_t{3});  // 콜백 중단
}

TEST(bplus_extreme_keys) {
  BPlusTree<int64_t, int64_t, 4> t;
  CHECK(t.insert(INT64_MIN, 1));
  CHECK(t.insert(INT64_MAX, 2));
  CHECK(t.insert(0, 3));
  CHECK_EQ(*t.find(INT64_MIN), int64_t{1});
  CHECK_EQ(*t.find(INT64_MAX), int64_t{2});
  CHECK(t.validate().ok());
}

TEST(bplus_height_grows_logarithmically) {
  BPlusTree<int64_t, uint64_t, 128> t;
  for (int64_t k = 1; k <= 200000; ++k) CHECK(t.insert(k, static_cast<uint64_t>(k)));
  CHECK(t.validate().ok());
  CHECK(t.height() <= 4);  // 128^3 > 200k
  CHECK(t.height() >= 2);
}
