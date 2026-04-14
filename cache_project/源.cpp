#include <iostream>
#include <string>
#include <random>
#include <atomic>
#include <thread>
#include <chrono>
#include "cache.h"
#include"map.h"
#include <iostream>
#include <vector>
#include <random>
#include <chrono>
#include <algorithm>
#include <fstream>
#include"cache.h"

using namespace my_cache;
using namespace std;
using namespace std::chrono;
using namespace my_cache;


// ==================== 正确的命中率测试 ====================

// 测试1: 先建立热度，再大量写入冷数据，最后测试热数据保留率
void test_hit_rate_build_then_burst() {
    const int CAPACITY = 300;
    const int HOT_KEYS = 30;
    const int BURST_KEYS = 1000;

    Map_Cache<int, string> hot_cache(CAPACITY);
    Cache<int, string> lru_cache(CacheType::LRU, CAPACITY);

    cout << "\n========== 测试1: 先建立热度再突发冷数据 ==========\n";
    cout << "容量: " << CAPACITY << ", 热key: " << HOT_KEYS << "个\n";
    cout << "阶段1: 建立热度 (10000次访问)\n";
    cout << "阶段2: 突发冷数据 (" << BURST_KEYS << "个新key)\n";
    cout << "阶段3: 测试热数据保留率\n\n";

    mt19937 rng(12345);

    // 阶段1: 建立热度 (只访问不淘汰)
    for (int i = 0; i < 10000; i++) {
        int key = rng() % HOT_KEYS;
        hot_cache.put(key, "value");
        hot_cache.get_shared(key);
        lru_cache.put(key, "value");
        lru_cache.get_shared(key);
    }

    // 阶段2: 突发大量冷key (触发淘汰)
    for (int i = 0; i < BURST_KEYS; i++) {
        int key = HOT_KEYS + i;
        hot_cache.put(key, "value");
        lru_cache.put(key, "value");
    }

    // 阶段3: 测试热key保留率 (只读不写)
    int hot_retained = 0, lru_retained = 0;
    for (int i = 0; i < HOT_KEYS; i++) {
        if (hot_cache.get_shared(i) != nullptr) hot_retained++;
        if (lru_cache.get_shared(i) != nullptr) lru_retained++;
    }

    cout << "热数据保留数量:\n";
    cout << "热度感知缓存: " << hot_retained << "/" << HOT_KEYS
        << " (" << (hot_retained * 100.0 / HOT_KEYS) << "%)\n";
    cout << "传统LRU缓存: " << lru_retained << "/" << HOT_KEYS
        << " (" << (lru_retained * 100.0 / HOT_KEYS) << "%)\n";
}

// 测试2: Zipf分布长期运行后的命中率
void test_hit_rate_zipf_long() {
    const int CAPACITY = 1000;
    const int TOTAL_KEYS = 4000;
    const int TRAIN_OPS = 50000;   // 训练阶段
    const int TEST_OPS = 10000;    // 测试阶段

    Map_Cache<int, string> hot_cache(CAPACITY);
    Cache<int, string> lru_cache(CacheType::LRU, CAPACITY);

    cout << "\n========== 测试2: Zipf分布长期运行 ==========\n";
    cout << "容量: " << CAPACITY << ", 总key: " << TOTAL_KEYS << "\n";
    cout << "训练: " << TRAIN_OPS << "次, 测试: " << TEST_OPS << "次\n";
    cout << "访问分布: 80%访问集中在前20%的key\n\n";

    mt19937 rng(45678);

    // 训练阶段 (让缓存状态稳定)
    for (int i = 0; i < TRAIN_OPS; i++) {
        int key;
        if (rng() % 100 < 80) {
            key = rng() % (TOTAL_KEYS / 5);  // 热区
        }
        else {
            key = (TOTAL_KEYS / 5) + rng() % (TOTAL_KEYS * 4 / 5);  // 冷区
        }
        hot_cache.put(key, "value");
        hot_cache.get_shared(key);
        lru_cache.put(key, "value");
        lru_cache.get_shared(key);
    }

    // 测试阶段 (只读，统计命中率)
    int hot_hits = 0, lru_hits = 0;
    for (int i = 0; i < TEST_OPS; i++) {
        int key;
        if (rng() % 100 < 80) {
            key = rng() % (TOTAL_KEYS / 5);
        }
        else {
            key = (TOTAL_KEYS / 5) + rng() % (TOTAL_KEYS * 4 / 5);
        }

        if (hot_cache.get_shared(key) != nullptr) hot_hits++;
        if (lru_cache.get_shared(key) != nullptr) lru_hits++;
    }

    cout << "命中率:\n";
    cout << "热度感知缓存: " << (hot_hits * 100.0 / TEST_OPS) << "%\n";
    cout << "传统LRU缓存: " << (lru_hits * 100.0 / TEST_OPS) << "%\n";
}

// 测试3: 访问模式突变
void test_hit_rate_pattern_shift() {
    const int CAPACITY = 500;

    Map_Cache<int, string> hot_cache(CAPACITY);
    Cache<int, string> lru_cache(CacheType::LRU, CAPACITY);

    cout << "\n========== 测试3修正: 测命中率 ==========\n";
    cout << "容量: " << CAPACITY << "\n";
    cout << "模式: A组热点 → B组热点 → 混合访问测试\n\n";

    mt19937 rng(99999);

    // 阶段1: A组热点
    for (int i = 0; i < 10000; i++) {
        int key = rng() % 100;
        hot_cache.put(key, "value");
        hot_cache.get_shared(key);
        lru_cache.put(key, "value");
        lru_cache.get_shared(key);
    }

    // 阶段2: B组热点 + 冷数据
    for (int i = 0; i < 5000; i++) {
        int key;
        if (i % 3 == 0) {
            key = 100 + rng() % 100;  // B组
        }
        else {
            key = 200 + rng() % 500;  // 冷数据
        }
        hot_cache.put(key, "value");
        hot_cache.get_shared(key);
        lru_cache.put(key, "value");
        lru_cache.get_shared(key);
    }

    // 阶段3: 混合访问测试 (A组30% + B组60% + 冷10%)
    int hot_hits = 0, lru_hits = 0;
    for (int i = 0; i < 10000; i++) {
        int key;
        int r = rng() % 100;
        if (r < 30) {
            key = rng() % 100;  // A组 30%
        }
        else if (r < 90) {
            key = 100 + rng() % 100;  // B组 60%
        }
        else {
            key = 200 + rng() % 500;  // 冷数据 10%
        }

        if (hot_cache.get_shared(key) != nullptr) hot_hits++;
        if (lru_cache.get_shared(key) != nullptr) lru_hits++;
    }

    cout << "混合访问命中率:\n";
    cout << "热度感知缓存: " << (hot_hits * 100.0 / 10000) << "%\n";
    cout << "传统LRU缓存: " << (lru_hits * 100.0 / 10000) << "%\n";
}

// 测试4: 周期性热点
void test_hit_rate_periodic_correct() {
    const int CAPACITY = 500;

    Map_Cache<int, string> hot_cache(CAPACITY);
    Cache<int, string> lru_cache(CacheType::LRU, CAPACITY);

    cout << "\n========== 测试4: 周期性热点 ==========\n";
    cout << "容量: " << CAPACITY << "\n";
    cout << "模拟: 每轮访问当前热点 + 大量冷数据\n\n";

    mt19937 rng(77777);

    // 多轮循环
    for (int round = 0; round < 5; round++) {
        // 当前热点
        for (int i = 0; i < 1000; i++) {
            int key = round * 20 + (i % 20);  // 本轮热点
            hot_cache.put(key, "value");
            hot_cache.get_shared(key);
            lru_cache.put(key, "value");
            lru_cache.get_shared(key);
        }

        // 冷数据冲刷
        for (int i = 0; i < 500; i++) {
            int key = 100 + (round * 100) + i;
            hot_cache.put(key, "value");
            lru_cache.put(key, "value");
        }
    }

    // 测试第一轮热点(0-19)的保留情况
    int hot_retained = 0, lru_retained = 0;
    for (int i = 0; i < 20; i++) {
        if (hot_cache.get_shared(i) != nullptr) hot_retained++;
        if (lru_cache.get_shared(i) != nullptr) lru_retained++;
    }

    cout << "5轮后，第一轮热点(0-19)保留:\n";
    cout << "热度感知缓存: " << hot_retained << "/20\n";
    cout << "传统LRU缓存: " << lru_retained << "/20 (应该为0)\n";
}
// ==================== 测试5: 关联访问 ====================
void test_associative_access() {
    const int CAPACITY = 80;
    const int TRAIN_OPS = 20000;
    const int TEST_OPS = 10000;

    Map_Cache<int, string> hot_cache(CAPACITY);
    Cache<int, string> lru_cache(CacheType::LRU, CAPACITY);

    cout << "\n========== 测试5: 关联访问效果 ==========\n";
    cout << "容量: " << CAPACITY << "\n";
    cout << "模拟: 访问A后经常访问B，但B单独访问较少\n\n";

    mt19937 rng(88888);

    // 阶段1: 建立关联模式
    // 模式: A组(0-19) 和 B组(20-39) 关联
    // 访问A后，80%概率访问B
    cout << "阶段1: 建立关联访问模式 (20000次)\n";
    for (int i = 0; i < TRAIN_OPS; i++) {
        int a_key = rng() % 20;  // A组 0-19

        // 访问A
        hot_cache.put(a_key, "value");
        auto node_a = hot_cache.get_shared(a_key);
        lru_cache.put(a_key, "value");
        lru_cache.get_shared(a_key);

        // 80%概率访问关联的B
        if (rng() % 100 < 80) {
            int b_key = 20 + (a_key % 20);  // B组 20-39，与A对应
            hot_cache.put(b_key, "value");
            hot_cache.get_shared(b_key);
            lru_cache.put(b_key, "value");
            lru_cache.get_shared(b_key);
        }

        // 20%概率访问冷数据
        if (rng() % 100 < 20) {
            int cold_key = 40 + rng() % 200;
            hot_cache.put(cold_key, "value");
            hot_cache.get_shared(cold_key);
            lru_cache.put(cold_key, "value");
            lru_cache.get_shared(cold_key);
        }
    }

    // 阶段2: 大量冷数据冲刷
    cout << "阶段2: 冷数据冲刷 (500个新key)\n";
    for (int i = 0; i < 500; i++) {
        int cold_key = 300 + i;
        hot_cache.put(cold_key, "value");
        lru_cache.put(cold_key, "value");
    }

    // 阶段3: 只访问A，测试B是否被保留
    cout << "阶段3: 只访问A，检查B的保留情况\n";

    // 先访问几次A（建立关联）
    for (int i = 0; i < 10; i++) {
        int a_key = i % 20;
        hot_cache.get_shared(a_key);
        lru_cache.get_shared(a_key);
    }

    // 检查B组的保留
    int hot_b_retained = 0, lru_b_retained = 0;
    for (int i = 20; i < 40; i++) {
        if (hot_cache.get_shared(i) != nullptr) hot_b_retained++;
        if (lru_cache.get_shared(i) != nullptr) lru_b_retained++;
    }

    cout << "B组(关联数据)保留数量:\n";
    cout << "热度感知缓存: " << hot_b_retained << "/20\n";
    cout << "传统LRU缓存: " << lru_b_retained << "/20\n";

    // 阶段4: 测试命中率（混合访问）
    cout << "\n阶段4: 混合访问命中率测试\n";
    int hot_hits = 0, lru_hits = 0;
    for (int i = 0; i < TEST_OPS; i++) {
        int key;
        int r = rng() % 100;
        if (r < 40) {
            key = rng() % 20;  // A组 40%
        }
        else if (r < 70) {
            key = 20 + rng() % 20;  // B组 30%
        }
        else {
            key = 40 + rng() % 200;  // 冷数据 30%
        }

        if (hot_cache.get_shared(key) != nullptr) hot_hits++;
        if (lru_cache.get_shared(key) != nullptr) lru_hits++;
    }

    cout << "关联访问场景命中率:\n";
    cout << "热度感知缓存: " << (hot_hits * 100.0 / TEST_OPS) << "%\n";
    cout << "传统LRU缓存: " << (lru_hits * 100.0 / TEST_OPS) << "%\n";
}

// ==================== 测试6: 强关联链 ====================
void test_strong_association_chain() {
    const int CAPACITY = 100;

    Map_Cache<int, string> hot_cache(CAPACITY);
    Cache<int, string> lru_cache(CacheType::LRU, CAPACITY);

    cout << "\n========== 测试6: 强关联链 ==========\n";
    cout << "容量: " << CAPACITY << "\n";
    cout << "模拟: A→B→C 访问链，但C很少被直接访问\n\n";

    mt19937 rng(66666);

    // 建立关联链: 0→10→20
    cout << "建立关联链: 访问0后80%访问10，访问10后80%访问20\n";
    for (int i = 0; i < 15000; i++) {
        // 访问链头
        hot_cache.put(0, "value");
        auto node = hot_cache.get_shared(0);
        lru_cache.put(0, "value");
        lru_cache.get_shared(0);

        if (rng() % 100 < 80) {
            hot_cache.put(10, "value");
            hot_cache.get_shared(10);
            lru_cache.put(10, "value");
            lru_cache.get_shared(10);

            if (rng() % 100 < 80) {
                hot_cache.put(20, "value");
                hot_cache.get_shared(20);
                lru_cache.put(20, "value");
                lru_cache.get_shared(20);
            }
        }

        // 冷数据
        if (rng() % 100 < 30) {
            int cold = 30 + rng() % 100;
            hot_cache.put(cold, "value");
            lru_cache.put(cold, "value");
        }
    }

    // 冲刷
    for (int i = 0; i < 200; i++) {
        int cold = 200 + i;
        hot_cache.put(cold, "value");
        lru_cache.put(cold, "value");
    }

    // 只访问链头，测试链尾是否被保护
    for (int i = 0; i < 20; i++) {
        hot_cache.get_shared(0);
        lru_cache.get_shared(0);
    }

    cout << "只访问链头(0)后，链尾(20)的保留:\n";
    bool hot_has_20 = (hot_cache.get_shared(20) != nullptr);
    bool lru_has_20 = (lru_cache.get_shared(20) != nullptr);

    cout << "热度感知缓存: " << (hot_has_20 ? "保留" : "淘汰") << "\n";
    cout << "传统LRU缓存: " << (lru_has_20 ? "保留" : "淘汰") << "\n";
}

// ==================== 测试7: 无关联对照组 ====================
void test_no_association_control() {
    const int CAPACITY = 120;

    Map_Cache<int, string> hot_cache(CAPACITY);
    Cache<int, string> lru_cache(CacheType::LRU, CAPACITY);

    cout << "\n========== 测试7: 无关联对照组 ==========\n";
    cout << "容量: " << CAPACITY << "\n";
    cout << "模拟: A和B访问量相同，但无关联\n\n";

    mt19937 rng(77777);

    // 独立访问，无关联
    for (int i = 0; i < 20000; i++) {
        int key;
        if (rng() % 100 < 50) {
            key = rng() % 20;  // A组
        }
        else {
            key = 20 + rng() % 20;  // B组
        }
        hot_cache.put(key, "value");
        hot_cache.get_shared(key);
        lru_cache.put(key, "value");
        lru_cache.get_shared(key);
    }

    // 冷数据冲刷
    for (int i = 0; i < 300; i++) {
        int cold = 100 + i;
        hot_cache.put(cold, "value");
        lru_cache.put(cold, "value");
    }

    // 只访问A组
    for (int i = 0; i < 50; i++) {
        hot_cache.get_shared(i % 20);
        lru_cache.get_shared(i % 20);
    }

    // 检查B组
    int hot_b = 0, lru_b = 0;
    for (int i = 20; i < 40; i++) {
        if (hot_cache.get_shared(i) != nullptr) hot_b++;
        if (lru_cache.get_shared(i) != nullptr) lru_b++;
    }

    cout << "无关联时，只访问A组后B组保留:\n";
    cout << "热度感知缓存: " << hot_b << "/20\n";
    cout << "传统LRU缓存: " << lru_b << "/20\n";
}

void test_mixed_qps(int thread_num = 16, int seconds = 10, int write_ratio = 20) {
    using namespace std::chrono;
    //Cache<int, std::string> cache(CacheType::LRU, 100000);
    ShardedCache<int, std::string> cache(100000);
    //Map_Cache<int, std::string> cache(100000);

    // 预热数据
    for (int i = 0; i < 10000; i++) {
        cache.put(i, "value_" + std::to_string(i));
    }

    std::atomic<uint64_t> total_reads{ 0 };
    std::atomic<uint64_t> total_writes{ 0 };
    std::atomic<bool> stop{ false };
    std::vector<std::thread> threads;

    auto start = steady_clock::now();

    for (int t = 0; t < thread_num; t++) {
        threads.emplace_back([&, t]() {
            std::mt19937 rng(t + 12345);
            std::uniform_int_distribution<int> key_dist(0, 19999);
            std::uniform_int_distribution<int> op_dist(0, 99);

            while (!stop) {
                int key = key_dist(rng);

                if (op_dist(rng) < write_ratio) {
                    // 写操作
                    std::string s(1000, 'x');  // 1000 个 'x'
                    cache.put(key, std::move(s));
                    total_writes++;
                }
                else {
                    // 读操作
                    auto node = cache.get_shared(key);
                    if (node) total_reads++;
                }
            }
            });
    }

    std::this_thread::sleep_for(seconds * 1s);
    stop = true;

    for (auto& th : threads) th.join();

    auto elapsed = duration<double>(steady_clock::now() - start).count();
    uint64_t total_ops = total_reads + total_writes;

    std::cout << "\n=== 混合读写 QPS 测试 ===\n";
    std::cout << "线程数: " << thread_num << "\n";
    std::cout << "写比例: " << write_ratio << "%\n";
    std::cout << "运行时间: " << elapsed << " 秒\n";
    std::cout << "总操作数: " << total_ops << "\n";
    std::cout << "总 QPS: " << (uint64_t)(total_ops / elapsed) << "\n";
    std::cout << "读 QPS: " << (uint64_t)(total_reads / elapsed) << "\n";
    std::cout << "写 QPS: " << (uint64_t)(total_writes / elapsed) << "\n";
}
void test_burst_write(int seconds = 5) {
    using namespace std::chrono;
    //Cache<int, std::string> cache(CacheType::LRU, 10000);
    Map_Cache<int, std::string> cache(100000);  // 小容量，测试背压

    std::atomic<uint64_t> total_writes{ 0 };
    std::atomic<bool> stop{ false };
    std::vector<std::thread> threads;

    auto start = steady_clock::now();

    // 16 线程疯狂写入
    for (int t = 0; t < 16; t++) {
        threads.emplace_back([&, t]() {
            std::mt19937 rng(t);
            std::uniform_int_distribution<int> dist(0, 100000000);

            while (!stop) {
                cache.put(dist(rng), "value");
                total_writes++;
            }
            });
    }

    // 每秒输出一次缓存大小
    for (int i = 0; i < seconds; i++) {
        std::this_thread::sleep_for(1s);
        std::cout << "[" << i + 1 << "s] 缓存大小: "
            << cache.hashmap.size << " / 1000\n";
    }

    stop = true;
    for (auto& th : threads) th.join();

    auto elapsed = duration<double>(steady_clock::now() - start).count();

    std::cout << "\n=== 突发写入测试 ===\n";
    std::cout << "运行时间: " << elapsed << " 秒\n";
    std::cout << "总写入数: " << total_writes << "\n";
    std::cout << "写入 QPS: " << (uint64_t)(total_writes / elapsed) << "\n";
    std::cout << "最终缓存大小: " << cache.hashmap.size << " / 1000\n";
}
void test_read_qps(int thread_num = 16, int seconds = 10) {
    using namespace std::chrono;

    Map_Cache<int, std::string> cache(100000);

    // 预热数据
    for (int i = 0; i < 10000; i++) {
        cache.put(i, "value_" + std::to_string(i));
    }

    std::atomic<uint64_t> total_ops{ 0 };
    std::atomic<bool> stop{ false };
    std::vector<std::thread> threads;

    auto start = steady_clock::now();

    // 启动读线程
    for (int t = 0; t < thread_num; t++) {
        threads.emplace_back([&, t]() {
            std::mt19937 rng(t + 12345);
            std::uniform_int_distribution<int> dist(0, 9999);
            uint64_t cnt = 0;

            while (!stop) {
                int key = dist(rng);
                auto node = cache.get_shared(key);
                if (node) cnt++;
            }
            total_ops += cnt;
            });
    }

    // 运行指定时间
    std::this_thread::sleep_for(seconds * 1s);
    stop = true;

    for (auto& th : threads) th.join();

    auto elapsed = duration<double>(steady_clock::now() - start).count();

    std::cout << "=== 纯读 QPS 测试 ===\n";
    std::cout << "线程数: " << thread_num << "\n";
    std::cout << "运行时间: " << elapsed << " 秒\n";
    std::cout << "总操作数: " << total_ops << "\n";
    std::cout << "QPS: " << (uint64_t)(total_ops / elapsed) << "\n";
}

void test_comprehensive_10k() {
    const int CAPACITY = 3000;  // 改为 10000
    //Map_Cache<int, std::string> cache(CAPACITY);
    Cache<int, std::string> cache(CacheType::LRU, CAPACITY);
    std::cout << "\n========== 综合测试 (容量 "<< CAPACITY <<") ==========\n";
    std::cout << "所有测试在同一个缓存实例上连续执行\n\n";

    std::mt19937 rng(12345);

    // 阶段1: Zipf稳态
    std::cout << "[阶段1] Zipf稳态 (50000次)\n";
    for (int i = 0; i < 50000; i++) {
        int key;
        if (rng() % 100 < 80) key = rng() % 2000;  // 热key范围扩大
        else key = 2000 + rng() % 8000;
        cache.put(key, "value");
        cache.get_shared(key);
    }

    // 阶段2: 突发冷数据冲刷
    std::cout << "[阶段2] 突发冷数据 (5000个新key)\n";
    for (int i = 0; i < 5000; i++) {
        cache.put(10000 + i, "cold");
    }

    // 阶段3: 关联访问模式
    std::cout << "[阶段3] 关联访问 (20000次, A->B)\n";
    for (int i = 0; i < 20000; i++) {
        int a = rng() % 50;
        cache.put(a, "A");
        cache.get_shared(a);
        if (rng() % 100 < 80) {
            int b = 50 + a;
            cache.put(b, "B");
            cache.get_shared(b);
        }
    }

    // 阶段4: 模式突变
    std::cout << "[阶段4] 模式突变 (新热点)\n";
    for (int i = 0; i < 10000; i++) {
        int key = 200 + rng() % 50;
        cache.put(key, "new_hot");
        cache.get_shared(key);
    }

    // 阶段5: 混合测试
    std::cout << "[阶段5] 混合测试 (10000次，统计命中率)\n";
    int hits = 0;
    for (int i = 0; i < 10000; i++) {
        int key;
        int r = rng() % 100;
        if (r < 30) key = rng() % 2000;
        else if (r < 50) key = 200 + rng() % 50;
        else if (r < 70) key = rng() % 50;
        else if (r < 90) key = 50 + rng() % 50;
        else key = 10000 + rng() % 5000;

        if (cache.get_shared(key) != nullptr) hits++;
    }

    std::cout << "综合命中率: " << (hits * 100.0 / 10000) << "%\n";
}

// ==================== 主函数 ====================
int main() {
    //test_comprehensive_10k();
    //return 0;
    //test_read_qps(16, 10);
    test_mixed_qps(8, 20, 20);
    //test_burst_write(10);
    cout << "╔══════════════════════════════════════════════╗\n";
    cout << "║       缓存淘汰准确度对比测试 (修正版)         ║\n";
    cout << "║     热度感知 vs 传统LRU                       ║\n";
    cout << "╚══════════════════════════════════════════════╝\n";

    test_hit_rate_build_then_burst();
    test_hit_rate_zipf_long();
    test_hit_rate_pattern_shift();
    test_hit_rate_periodic_correct();
    test_associative_access();
    test_strong_association_chain();
    test_no_association_control();
    cout << "\n==========================================\n";

    return 0;
}