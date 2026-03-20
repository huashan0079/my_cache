#include <iostream>
#include <string>
#include <random>
#include <atomic>
#include <thread>
#include <chrono>
#include "cache.h"
using namespace std;
using namespace std::chrono;
using namespace my_cache;

void test_performance() {

	std::cout << "\n----- Performance Test -----\n";

	const int CACHE_SIZE = 3000;
	const int TEST_COUNT = 1000000;  // 100万次操作

	my_cache::Cache<int, std::string> cache(my_cache::CacheType::LRU, CACHE_SIZE);

	// 预热：放入5000个数据
	for (int i = 0; i < 5000; ++i) {
		cache.put(i, "value" + std::to_string(i));
	}

	// 测试1：顺序读
	auto start = std::chrono::steady_clock::now();
	for (int i = 0; i < TEST_COUNT; ++i) {
		std::string value;
		cache.get(i % 8000);
	}
	auto end = std::chrono::steady_clock::now();
	auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
	std::cout << "Sequential read: " << TEST_COUNT * 1000 / ms << " QPS\n";

	// 测试2：随机读
	std::random_device rd;
	std::mt19937 gen(rd());
	std::uniform_int_distribution<> dist(0, 15000);

	start = std::chrono::steady_clock::now();
	for (int i = 0; i < TEST_COUNT; ++i) {
		std::string value;
		cache.get(dist(gen));
	}
	end = std::chrono::steady_clock::now();
	ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
	std::cout << "Random read: " << TEST_COUNT * 1000 / ms << " QPS\n";

	// 命中率统计
	int hits = 0, total = 0;
	for (int i = 0; i < 10000; ++i) {
		std::string value;
		if (cache.get(i % 1500)) hits++;
		total++;
	}
	std::cout << "Hit rate: " << (hits * 100.0 / total) << "%\n";
}

// 多线程测试
void test_multithread() {
	std::cout << "\n----- Multi-thread Test -----\n";

	// 容量10000，key范围20000（50%命中率）
	my_cache::Cache<int, std::string> cache(my_cache::CacheType::LRU, 10000);

	// 填满缓存
	for (int i = 0; i < 10000; ++i) {
		cache.put(i, "value" + std::to_string(i));
	}

	const int THREADS = 4;
	const int OPS_PER_THREAD = 50000;  // 总200万次
	std::vector<std::thread> threads;

	auto start = std::chrono::steady_clock::now();

	for (int t = 0; t < THREADS; ++t) {
		threads.emplace_back([&]() {
			std::random_device rd;
			std::mt19937 gen(rd());
			std::uniform_int_distribution<> dist(0, 19999);  // 50%命中

			for (int i = 0; i < OPS_PER_THREAD; ++i) {
				int key = dist(gen);

				if (i % 10 == 0) {  // 10%写
					cache.put(key, "new" + std::to_string(key));
				}
				else {  // 90%读
					std::string value;
					cache.get(key);
				}
			}
			});
	}

	for (auto& t : threads) t.join();

	auto end = std::chrono::steady_clock::now();
	auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
	int total_ops = THREADS * OPS_PER_THREAD;

	std::cout << "Threads: " << THREADS << "\n";
	std::cout << "Total ops: " << total_ops << "\n";
	std::cout << "Time: " << ms << " ms\n";
	std::cout << "QPS: " << total_ops * 1000 / ms << "\n";
}

int main() {
	test_performance();
	test_multithread();
    
    return 0;
}