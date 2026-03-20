#pragma once
#include<iostream>
#include<unordered_map>
#include<mutex>
#include<condition_variable>
namespace my_cache {

	enum class CacheType {
		LRU
	};

	template<typename K, typename V>
	class Interface_Cache {
	public:
		Interface_Cache() = default;
		virtual ~Interface_Cache() = default;
		Interface_Cache(const Interface_Cache& ic) = delete;
		Interface_Cache& operator=(const Interface_Cache& ic) = delete;

		virtual bool put(const K& key, V&& value) = 0;
		virtual V get(const K& key) = 0;
		virtual bool clear(const K& key) = 0;
		virtual void clear_all() = 0;
	};


	template<typename K, typename V>
	class LRU_Cache : public Interface_Cache<K, V> {
	public:
		LRU_Cache(const LRU_Cache& lc) = delete;
		LRU_Cache& operator=(const LRU_Cache& lc) = delete;
		LRU_Cache(int cap) : capacity(cap) {
			if (cap <= 0) throw std::invalid_argument("capacity must be positive");
		}
		~LRU_Cache() {
			clear_all();
		}
		bool put(const K& key, V&& value) {
			std::lock_guard<std::mutex> lock(mutex_);
			auto it = cache_map.find(key);
			if (it != cache_map.end()) {
				// 存在：更新字段类型，移到头部
				it->second->second = std::forward<V>(value);
				cache_list.splice(cache_list.begin(), cache_list, it->second);

			}
			else {
				if (cache_list.size() >= capacity && !cache_list.empty()) {
					auto last_node = cache_list.back();
					cache_map.erase(last_node.first);
					cache_list.pop_back();
				}
				cache_list.emplace_front(key, std::forward<V>(value));
				cache_map[key] = cache_list.begin();
			}
			return true;
		}


		V get(const K& key) {

			std::lock_guard<std::mutex> lock(mutex_);
			auto it = cache_map.find(key);
			if (it == cache_map.end()) {
				return V();
			}

			cache_list.splice(cache_list.begin(), cache_list, it->second);
			return it->second->second;
		}

		bool clear(const K& key) {
			std::lock_guard<std::mutex> lock(mutex_);
			auto it = cache_map.find(key);
			if (it == cache_map.end()) {
				std::cout << key << " cache is not exist" << std::endl;
				return false;
			}
			cache_list.erase(it->second);
			//从哈希表中删除映射
			cache_map.erase(it);
			std::cout << key << " cache is deleted" << std::endl;
			return true;
		}

		void clear_all() {
			std::lock_guard<std::mutex> lock(mutex_);
			cache_list.clear();
			cache_map.clear();
		}
	private:
		int capacity;
		std::mutex mutex_;
		std::list<std::pair<K, V>> cache_list;
		std::unordered_map<K, typename std::list<std::pair<K, V>>::iterator> cache_map;
	};




	template <typename T, typename = void>
	struct is_hashable : std::false_type {};

	// 特化模板：检测是否存在std::hash<T>::operator()
	template <typename T>
	struct is_hashable<T, std::void_t<decltype(std::hash<T>()(std::declval<T>()))>>
		: std::true_type {
	};

	// 便捷的常量表达式
	template <typename T>
	constexpr bool is_hashable_v = is_hashable<T>::value;



	template<typename K, typename V, typename Hash = std::hash<K>>
	class Cache {
		Hash hasher;
		std::atomic<uint64_t> version{ 0 };
		static_assert(is_hashable_v<K> || !std::is_same_v<Hash, std::hash<K>>,
			"Key type K must be hashable. Either:\n"
			"1. Use a built-in hashable type (int, string, etc.)\n"
			"2. Specialize std::hash<K>\n"
			"3. Provide a custom Hash template parameter");
	public:

		class DataShell {
			uint64_t version_;
			V value;  // 直接存储值，但通过移动语义避免拷贝
			const Cache* cache;
		public:
			DataShell() = delete;
			DataShell(const DataShell&) = delete;
			DataShell& operator=(const DataShell&) = delete;

			DataShell(V&& val, uint64_t ver, const Cache* cache)
				: version_(ver), value(std::forward<V>(val)), cache(cache) {
			}

			const V* get() const {
				if (cache->version > version_) {
					return nullptr;
				}
				return &value;
			}

		};


		Cache(CacheType type, size_t total_capacity) :
			Cache(type, total_capacity, auto_calc_shard_count(total_capacity)) {

		}
		Cache(CacheType type, size_t total_capacity, size_t shard_count) {
			if (total_capacity <= 0 || shard_count <= 0 || total_capacity < shard_count)
				throw std::invalid_argument("total_capacity and shard_count must be positive and total_capacity>=shard_count");
			this->shard_count = shard_count;
			float redundancy = calculate_redundancy(total_capacity, shard_count);
			int each_capacity = total_capacity * redundancy / shard_count;
			if (type == CacheType::LRU) {
				for (size_t i = 0; i < shard_count; ++i) {
					shards.emplace_back(std::make_unique<LRU_Cache<K, std::shared_ptr<DataShell>>>(each_capacity));
				}
			}
		}
		~Cache() {
			clear_all();
		}
		void put(const K& key, V&& value) {
			size_t shard_index = hasher(key) % shard_count;
			auto shell = std::make_shared<DataShell>(
				std::forward<V>(value),
				version,
				this
			);
			shards[shard_index]->put(key, shell);
		}
		auto get(const K& key) {
			size_t shard_index = hasher(key) % shard_count;
			return shards[shard_index]->get(key);
		}

		void update_all() {
			version++;
		}

		bool clear(const K& key) {
			size_t shard_index = hasher(key) % shard_count;
			return shards[shard_index]->clear(key);
		}
		void clear_all() {
			for (auto& shard : shards) {
				shard->clear_all();
			}
		}
	private:
		size_t shard_count;
		std::vector<std::unique_ptr<Interface_Cache<K, std::shared_ptr<DataShell>>>> shards;//每个分片一个缓存
		static int auto_calc_shard_count(size_t total_capacity) {
			if (total_capacity <= 1000)  return 4;       // 小缓存：单分片0~250
			if (total_capacity <= 5000)  return 16;      // 中等：单分片250~312.5
			if (total_capacity <= 20000) return 32;      // 较大：单分片312.5~625
			if (total_capacity <= 50000) return 64;      // 大：单分片625~781.25
			if (total_capacity <= 250000) return 128;    // 超大：单分片781.25~1953
			if (total_capacity <= 500000) return 256;    // 巨型：单分片1953~1953
			return 512;                                  // 超巨型：单分片<2000                     
		}

		float calculate_redundancy(size_t total_capacity, int shard_count) {
			float avg_per_shard = (float)total_capacity / shard_count;
			if (avg_per_shard < 5) {
				return 2.5;  // 极小缓存，需要2.5倍冗余
			}
			else if (avg_per_shard < 20) {
				return 2.0;  // 小缓存，2倍冗余
			}
			else if (avg_per_shard < 100) {
				return 1.5;  // 中等缓存，1.5倍
			}
			else if (avg_per_shard < 500) {
				return 1.2;  // 大缓存，1.2倍
			}
			else {
				return 1.1;  // 超大缓存，1.1倍足够
			}
		}
	};
};

