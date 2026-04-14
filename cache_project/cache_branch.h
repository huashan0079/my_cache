#pragma once
#pragma once
#include <iostream>
#include <unordered_map>
#include <mutex>
#include <condition_variable>
#include <list>
#include <vector>
#include <atomic>
#include <type_traits>
#include <memory>
#include <utility>
#include <algorithm>
#include <thread>
#include <queue>
#include <deque>
#include <fstream>
#include <cmath>
#include <cstring>
namespace my_cache
{

	class MLPModel
	{
	private:
		// 第一层: 8 → 32
		float w1[8][32];
		float b1[32];

		// BatchNorm 层 (第一层后)
		float bn_gamma[32];
		float bn_beta[32];
		float bn_running_mean[32];
		float bn_running_var[32];

		// 第二层: 32 → 32
		float w2[32][32];
		float b2[32];

		// 第三层: 32 → 1
		float w3[32];
		float b3;

		// 标准化参数（输入特征标准化）
		float mean[8];
		float scale[8]; // 1 / std

		inline float relu(float x) { return x > 0 ? x : 0; }
		inline float sigmoid(float x) { return 1.0f / (1.0f + std::exp(-x)); }

		// BatchNorm 推理
		inline float batch_norm(float x, float gamma, float beta, float mean, float var)
		{
			float inv_std = 1.0f / std::sqrt(var + 1e-5f);
			return gamma * (x - mean) * inv_std + beta;
		}

	public:
		static MLPModel& instance()
		{
			static MLPModel model;
			return model;
		}
		// 加载模型权重（二进制格式）
		bool load(const std::string& prefix)
		{
			std::cout << "开始加载模型，路径: " << prefix << std::endl;

			auto load_bin = [&](const std::string& name, void* ptr, size_t size) -> bool
				{
					std::string full_path = prefix + name;
					std::ifstream f(full_path, std::ios::binary);
					if (!f)
					{
						std::cerr << "  文件不存在: " << full_path << std::endl;
						return false;
					}
					f.read(reinterpret_cast<char*>(ptr), size);
					bool ok = f.good();
					if (ok)
					{
						std::cout << "  加载成功: " << name << " (" << size << " bytes)" << std::endl;
					}
					else
					{
						std::cerr << "  加载失败: " << name << std::endl;
					}
					return ok;
				};

			bool ok = true;
			ok &= load_bin("w1.bin", w1, sizeof(w1));
			ok &= load_bin("b1.bin", b1, sizeof(b1));
			ok &= load_bin("bn_gamma.bin", bn_gamma, sizeof(bn_gamma));
			ok &= load_bin("bn_beta.bin", bn_beta, sizeof(bn_beta));
			ok &= load_bin("bn_mean.bin", bn_running_mean, sizeof(bn_running_mean));
			ok &= load_bin("bn_var.bin", bn_running_var, sizeof(bn_running_var));
			ok &= load_bin("w2.bin", w2, sizeof(w2));
			ok &= load_bin("b2.bin", b2, sizeof(b2));
			ok &= load_bin("w3.bin", w3, sizeof(w3));
			ok &= load_bin("b3.bin", &b3, sizeof(b3));
			ok &= load_bin("mean.bin", mean, sizeof(mean));
			ok &= load_bin("scale.bin", scale, sizeof(scale));

			if (ok)
			{
				std::cout << "模型加载成功！" << std::endl;
				// 打印前几个权重值验证
				std::cout << "w1[0][0] = " << w1[0][0] << std::endl;
				std::cout << "b1[0] = " << b1[0] << std::endl;
				std::cout << "mean[0] = " << mean[0] << std::endl;
				std::cout << "scale[0] = " << scale[0] << std::endl;
			}
			else
			{
				std::cerr << "模型加载失败！" << std::endl;
			}
			return ok;
		}

		// 单节点推理
		float predict(const std::vector<float>& feats)
		{
			// 1. 输入标准化
			float x[8];
			for (int i = 0; i < 8; ++i)
			{
				x[i] = (feats[i] - mean[i]) * scale[i];
			}

			// 2. 第一层: 8 → 32
			float h1[32];
			for (int j = 0; j < 32; ++j)
			{
				float sum = b1[j];
				for (int k = 0; k < 8; ++k)
				{
					sum += x[k] * w1[k][j];
				}
				h1[j] = sum;
			}

			// 3. BatchNorm + ReLU
			float h1_norm[32];
			for (int j = 0; j < 32; ++j)
			{
				float bn_out = batch_norm(h1[j], bn_gamma[j], bn_beta[j],
					bn_running_mean[j], bn_running_var[j]);
				h1_norm[j] = relu(bn_out);
			}

			// 4. 第二层: 32 → 32
			float h2[32];
			for (int j = 0; j < 32; ++j)
			{
				float sum = b2[j];
				for (int k = 0; k < 32; ++k)
				{
					sum += h1_norm[k] * w2[k][j];
				}
				h2[j] = relu(sum);
			}

			// 5. 第三层: 32 → 1
			float sum = b3;
			for (int j = 0; j < 32; ++j)
			{
				sum += h2[j] * w3[j];
			}

			return sigmoid(sum);
		}

		// 批量推理
		std::vector<float> predict_batch(const std::vector<std::vector<float>>& feats)
		{
			std::vector<float> scores(feats.size());
			for (size_t i = 0; i < feats.size(); ++i)
			{
				scores[i] = predict(feats[i]);
			}
			return scores;
		}
	};

	enum class CacheType
	{
		LRU,
		MLP_LRU
	};

	template <typename K, typename V>
	class Interface_Cache
	{
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
	template <typename K, typename V>
	class Worker;

	static uint64_t now()
	{
		return std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::system_clock::now().time_since_epoch())
			.count();
	}

	/*template <typename K, typename V>
	class MLP_LRU_Cache : public Interface_Cache<K, V>
	{
		template <typename K1, typename V1>
		friend class Worker;

	public:
		MLP_LRU_Cache(const MLP_LRU_Cache& lc) = delete;
		MLP_LRU_Cache& operator=(const MLP_LRU_Cache& lc) = delete;
		MLP_LRU_Cache(int cap, Worker<K, V>* worker_)
			: capacity(cap), worker(worker_)
		{
			if (cap <= 0)
				throw std::invalid_argument("capacity must be positive");
		}
		~MLP_LRU_Cache()
		{
			clear_all();
		}
		bool put(const K& key, V&& value)
		{
			// std::cout << "put: this = " << this << std::endl;
			// std::cout << "put: &cache_list = " << &cache_list << std::endl;
			if (worker == nullptr)
			{
				// std::cerr << "错误: worker 是空指针!" << std::endl;
				return false;
			}
			else
			{
				// std::cout << "put: " << key << std::endl;
			}
			std::lock_guard<std::mutex> lock(mutex_);
			auto it = cache_map.find(key);
			if (it != cache_map.end())
			{
				// ???????????????????????
				V last = it->second->second;
				value->access_count_ = last->access_count_ + 1;
				value->last_access_time_ = now();
				value->insert_time_ = last->insert_time_;
				it->second->second = std::forward<V>(value);
				cache_list.splice(cache_list.begin(), cache_list, it->second);
			}
			else
			{

				if (cache_list.size() == capacity * 0.9 && !cache_list.empty())
				{
					if (!pending_.exchange(true))
					{
						worker->submit(this);
					}
				}
				if (cache_list.size() >= capacity * 0.99 && !cache_list.empty())
				{
					if (!pending_.exchange(true))
					{
						worker->submit(this);
					}
				}
				if (cache_list.size() >= capacity && !cache_list.empty())
				{
					// std::cout << "淘汰分支: size=" << cache_list.size() << ", empty=" << cache_list.empty() << std::endl;
					// std::cout << "淘汰分支: capacity=" << capacity << std::endl;

					// 再次检查
					if (cache_list.empty())
					{
						// std::cerr << "严重错误: empty() 返回 false 但 list 是空的!" << std::endl;
						return false;
					}

					// std::cout << "淘汰分支: 准备获取 back()" << std::endl;
					auto last_node = cache_list.back();
					// std::cout << "淘汰分支: back() 成功, key=" << last_node.first << std::endl;

					cache_map.erase(last_node.first);
					cache_list.pop_back();
					// std::cout << "淘汰分支: 删除完成" << std::endl;
				}
				value->insert_time_ = now();
				value->last_access_time_ = value->insert_time_;
				cache_list.emplace_front(key, std::forward<V>(value));
				cache_map[key] = cache_list.begin();
			}
			return true;
		}

		V get(const K& key)
		{

			std::lock_guard<std::mutex> lock(mutex_);
			auto it = cache_map.find(key);
			if (it == cache_map.end())
			{
				return V();
			}

			cache_list.splice(cache_list.begin(), cache_list, it->second);
			it->second->second->last_access_time_ = now();
			it->second->second->access_count_++;
			return it->second->second;
		}

		bool clear(const K& key)
		{
			std::lock_guard<std::mutex> lock(mutex_);
			auto it = cache_map.find(key);
			if (it == cache_map.end())
			{
				// std::cout << key << " cache is not exist" << std::endl;
				return false;
			}
			cache_list.erase(it->second);
			// ??????????????
			cache_map.erase(it);
			// std::cout << key << " cache is deleted" << std::endl;
			return true;
		}

		void clear_all()
		{
			std::lock_guard<std::mutex> lock(mutex_);
			cache_list.clear();
			cache_map.clear();
		}

	private:
		int capacity;
		std::atomic<bool> pending_{ false };
		Worker<K, V>* worker;
		std::mutex mutex_;
		std::list<std::pair<K, V>> cache_list;
		std::unordered_map<K, typename std::list<std::pair<K, V>>::iterator> cache_map;
	};*/

	/*template <typename K, typename V>
	class Worker
	{
	public:
		std::thread thread;
		std::mutex mutex;
		std::condition_variable cv;
		std::queue<MLP_LRU_Cache<K, V>*> pending_shards; // 待处理的分片
		std::atomic<bool> running{ true };
		std::atomic<bool> started{ false };

		Worker()
		{
			thread = std::thread([this]()
				{ loop(); });
		}

		// 启动工作线程（需要在所有对象初始化完成后调用）

		~Worker()
		{
			running = false;
			{
				std::lock_guard lock(mutex);
				cv.notify_one();
			}
			if (thread.joinable())
				thread.join();
		}

		// 提交任务
		void submit(MLP_LRU_Cache<K, V>* shard)
		{
			{
				std::lock_guard<std::mutex> lock(mutex);
				pending_shards.push(shard);
			}
			cv.notify_one();
		}

	private:
		struct temp
		{
			double self_density;
			double self_idle_ratio;
			K key;
			float label;
		};

		void process(MLP_LRU_Cache<K, V>* shard);

		void loop()
		{
			while (running)
			{
				std::unique_lock<std::mutex> lock(mutex); // ? 可以用于 wait
				cv.wait(lock, [this]
					{ return !pending_shards.empty() || !running; });

				if (!running)
					break;

				auto* shard = pending_shards.front();
				pending_shards.pop();
				lock.unlock(); // ? 处理前释放锁

				if (shard->cache_list.size() > shard->capacity * 0.9)
				{
					process(shard); // 无锁处理，不阻塞其他线程
				}
			}
		}
	};*/

	template <typename K, typename V>
	class LRU_Cache : public Interface_Cache<K, V>
	{
	public:
		LRU_Cache(const LRU_Cache& lc) = delete;
		LRU_Cache& operator=(const LRU_Cache& lc) = delete;
		LRU_Cache(int cap) : capacity(cap)
		{
			if (cap <= 0)
				throw std::invalid_argument("capacity must be positive");
		}
		~LRU_Cache()
		{
			clear_all();
		}
		bool put(const K& key, V&& value)
		{
			std::lock_guard<std::mutex> lock(mutex_);
			auto it = cache_map.find(key);
			if (it != cache_map.end())
			{
				// ???????????????????????
				it->second->second = std::forward<V>(value);
				cache_list.splice(cache_list.begin(), cache_list, it->second);
			}
			else
			{

				if (cache_list.size() >= capacity && !cache_list.empty())
				{
					auto last_node = cache_list.back();
					cache_map.erase(last_node.first);
					cache_list.pop_back();
				}
				cache_list.emplace_front(key, std::forward<V>(value));
				cache_map[key] = cache_list.begin();
			}
			return true;
		}

		V get(const K& key)
		{

			std::lock_guard<std::mutex> lock(mutex_);
			auto it = cache_map.find(key);
			if (it == cache_map.end())
			{
				return V();
			}

			cache_list.splice(cache_list.begin(), cache_list, it->second);
			return it->second->second;
		}

		bool clear(const K& key)
		{
			std::lock_guard<std::mutex> lock(mutex_);
			auto it = cache_map.find(key);
			if (it == cache_map.end())
			{
				//std::cout << key << " cache is not exist" << std::endl;
				return false;
			}
			cache_list.erase(it->second);
			// ??????????????
			cache_map.erase(it);
			//std::cout << key << " cache is deleted" << std::endl;
			return true;
		}

		void clear_all()
		{
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
	struct is_hashable : std::false_type
	{
	};

	// ?????壺?????????std::hash<T>::operator()
	template <typename T>
	struct is_hashable<T, std::void_t<decltype(std::hash<T>()(std::declval<T>()))>>
		: std::true_type
	{
	};

	// ????????????
	template <typename T>
	constexpr bool is_hashable_v = is_hashable<T>::value;

	template <typename K, typename V, typename Hash = std::hash<K>>
	class Cache
	{
		Hash hasher;
		std::atomic<uint64_t> version{ 0 };

	public:
		class DataShell
		{
			template <typename K1, typename V1>
			friend class MLP_LRU_Cache;
			template <typename K1, typename V1>
			friend class Worker;
			uint64_t version_;
			K key;
			V value; // ??洢????
			const Cache* cache;
			uint64_t access_count_;		// 访问次数
			uint64_t last_access_time_; // 最后访问时间
			uint64_t insert_time_;		// 插入时间
		public:
			DataShell() = delete;
			DataShell(const DataShell&) = delete;
			DataShell& operator=(const DataShell&) = delete;

			DataShell(uint64_t ver, const K& key_, V&& val, const Cache* cache)
				: version_(ver), key(key_), value(std::forward<V>(val)), cache(cache),
				access_count_(1), last_access_time_(0), insert_time_(0)
			{
			}

			const V* get() const
			{
				if (cache->version > version_)
				{
					return nullptr;
				}
				return &value;
			}
		};

		static_assert(is_hashable_v<K> || !std::is_same_v<Hash, std::hash<K>>,
			"Key type K must be hashable. Either:\n"
			"1. Use a built-in hashable type (int, string, etc.)\n"
			"2. Specialize std::hash<K>\n"
			"3. Provide a custom Hash template parameter");

		Cache(CacheType type, size_t total_capacity) : Cache(type, total_capacity, auto_calc_shard_count(total_capacity))
		{
		}
		Cache(CacheType type, size_t total_capacity, size_t shard_count)
		{
			if (total_capacity <= 0 || shard_count <= 0 || total_capacity < shard_count)
				throw std::invalid_argument("total_capacity and shard_count must be positive and total_capacity>=shard_count");
			this->shard_count = shard_count;
			float redundancy = calculate_redundancy(total_capacity, shard_count);
			int each_capacity = total_capacity * redundancy / shard_count;
			if (type == CacheType::LRU)
			{
				for (size_t i = 0; i < shard_count; ++i)
				{
					shards.emplace_back(std::make_unique<LRU_Cache<K, std::shared_ptr<DataShell>>>(each_capacity));
				}
			}
			/*else if (type == CacheType::MLP_LRU)
			{
				size_t thread_count = std::max((size_t)1, shard_count / 8);
				for (size_t i = 0; i < thread_count; ++i)
				{
					workers.emplace_back(std::make_unique<Worker<K, std::shared_ptr<DataShell>>>());
				}
				for (size_t i = 0; i < shard_count; ++i)
				{
					size_t worker_id = i % thread_count;
					shards.emplace_back(std::make_unique<MLP_LRU_Cache<K, std::shared_ptr<DataShell>>>(each_capacity, workers[worker_id].get()));
				}
				auto& model = MLPModel::instance();
				if (!model.load("D:/vs code/code/ml_model/models/"))
				{
					std::cout << "Failed to load MLP model weights!" << std::endl;
				}
				else
				{
					std::cout << "MLP model loaded successfully." << std::endl;
				}
			}*/
		}
		~Cache()
		{
			clear_all();
		}
		void put(const K& key, V&& value)
		{
			size_t shard_index = hasher(key) % shard_count;
			auto shell = std::make_shared<DataShell>(
				version,
				key,
				std::forward<V>(value),
				this);
			shards[shard_index]->put(key, std::move(shell));
		}
		auto get_shared(const K& key) const
		{
			size_t shard_index = hasher(key) % shard_count;
			return shards[shard_index]->get(key);
		}

		const V* get_ptr(const K& key) const
		{
			auto shell_ptr = get_shared(key);
			if (!shell_ptr)
				return nullptr;
			return shell_ptr->get();
		}

		V get(const K& key) const
		{
			auto shell_ptr = get_shared(key);
			if (!shell_ptr)
				return V();
			const V* val_ptr = shell_ptr->get();
			return val_ptr ? *val_ptr : V();
		}

		void update_all()
		{
			version++;
		}

		bool clear(const K& key)
		{
			size_t shard_index = hasher(key) % shard_count;
			return shards[shard_index]->clear(key);
		}
		void clear_all()
		{
			for (auto& shard : shards)
			{
				shard->clear_all();
			}
		}

	private:
		size_t shard_count;
		//std::vector<std::unique_ptr<Worker<K, std::shared_ptr<DataShell>>>> workers;
		std::vector<std::unique_ptr<Interface_Cache<K, std::shared_ptr<DataShell>>>> shards; // ?????????????
		static int auto_calc_shard_count(size_t total_capacity)
		{
			if (total_capacity <= 1000)
				return 4; // С???棺?????0~250
			if (total_capacity <= 5000)
				return 16; // ?е???????250~312.5
			if (total_capacity <= 20000)
				return 32; // ????????312.5~625
			if (total_capacity <= 50000)
				return 64; // ???????625~781.25
			if (total_capacity <= 250000)
				return 128; // ?????????781.25~1953
			if (total_capacity <= 500000)
				return 256; // ??????????1953~1953
			return 512;		// ????????????<2000
		}

		float calculate_redundancy(size_t total_capacity, int shard_count)
		{
			float avg_per_shard = (float)total_capacity / shard_count;
			if (avg_per_shard < 5)
			{
				return 2.5; // ??С???棬???2.5??????
			}
			else if (avg_per_shard < 20)
			{
				return 2.0; // С???棬2??????
			}
			else if (avg_per_shard < 100)
			{
				return 1.5; // ?е???棬1.5??
			}
			else if (avg_per_shard < 500)
			{
				return 1.2; // ???棬1.2??
			}
			else
			{
				return 1.1; // ?????棬1.1????
			}
		}
	};

	/*template <typename K, typename V>
	void Worker<K, V>::process(MLP_LRU_Cache<K, V>* shard)
	{
		std::vector<V> snapshot;
		std::vector<temp> vec_temp;
		{
			std::lock_guard<std::mutex> lock(shard->mutex_);

			size_t snap_size = shard->cache_list.size() * 0.7;

			auto it = shard->cache_list.rbegin();
			for (size_t i = 0; i < snap_size && it != shard->cache_list.rend(); ++i)
			{
				snapshot.emplace_back(it->second); // 复制 shared_ptr
				++it;
			}
		}
		int size = snapshot.size();
		for (int i = 0; i < size; ++i)
		{
			temp t{};
			t.key = snapshot[i]->key;
			int64_t lifetime = now() - snapshot[i]->insert_time_;
			int64_t idle = now() - snapshot[i]->last_access_time_;
			t.self_density = lifetime > 0 ? snapshot[i]->access_count_ / (double)lifetime : 0;
			t.self_idle_ratio = lifetime > 0 ? idle / (double)lifetime : 0;
			vec_temp.push_back(t);
		}

		int n = vec_temp.size();
		std::vector<std::vector<float>> features(n, std::vector<float>(8));
		int window_size = 5;
		// 只需要这 4 个队列！没有任何多余数组！
		std::deque<int> max_dq, min_dq;
		std::deque<int> max_iq, min_iq;

		double sum_dens = 0.0;
		double sum_idle = 0.0;

		for (int i = 0; i < n; ++i)
		{
			double sd = vec_temp[i].self_density;
			double si = vec_temp[i].self_idle_ratio;

			// 窗口滑动：移除超出左边界的元素
			auto shrink = [&](std::deque<int>& dq, int bound)
				{
					while (!dq.empty() && dq.front() < bound)
						dq.pop_front();
				};
			shrink(max_dq, i - window_size + 1);
			shrink(min_dq, i - window_size + 1);
			shrink(max_iq, i - window_size + 1);
			shrink(min_iq, i - window_size + 1);

			// 维护单调队列：O(1) max/min
			while (!max_dq.empty() && sd >= vec_temp[max_dq.back()].self_density)
				max_dq.pop_back();
			while (!min_dq.empty() && sd <= vec_temp[min_dq.back()].self_density)
				min_dq.pop_back();
			while (!max_iq.empty() && si >= vec_temp[max_iq.back()].self_idle_ratio)
				max_iq.pop_back();
			while (!min_iq.empty() && si <= vec_temp[min_iq.back()].self_idle_ratio)
				min_iq.pop_back();

			max_dq.push_back(i);
			min_dq.push_back(i);
			max_iq.push_back(i);
			min_iq.push_back(i);

			// 滑动和
			sum_dens += sd;
			sum_idle += si;
			if (i >= window_size)
			{
				sum_dens -= vec_temp[i - window_size].self_density;
				sum_idle -= vec_temp[i - window_size].self_idle_ratio;
			}

			// 当前窗口真正的 avg / max / min
			int wlen = std::min(i + 1, window_size);
			double avg_dens = sum_dens / wlen;
			double avg_idle = sum_idle / wlen;
			double max_d = vec_temp[max_dq.front()].self_density;
			double min_d = vec_temp[min_dq.front()].self_density;
			double max_i = vec_temp[max_iq.front()].self_idle_ratio;
			double min_i = vec_temp[min_iq.front()].self_idle_ratio;

			// 左右邻居
			double L = (i > 0) ? vec_temp[i - 1].self_density : sd;
			double R = (i < n - 1) ? vec_temp[i + 1].self_density : sd;

			// ========================
			// 你的 8 个特征，完全不动！
			// ========================
			features[i][0] = static_cast<float>(sd - avg_dens);
			features[i][1] = static_cast<float>(si - avg_idle);
			features[i][2] = static_cast<float>((max_d > min_d) ? (sd - min_d) / (max_d - min_d) : 0.0);
			features[i][3] = static_cast<float>((max_i > min_i) ? 1.0 - (si - min_i) / (max_i - min_i) : 0.5);
			features[i][4] = static_cast<float>(sd - (L + R) * 0.5);
			features[i][5] = static_cast<float>((R - sd) - (sd - L));
			features[i][6] = static_cast<float>((sd > L && sd > R) ? (sd - avg_dens) : 0.0);
			features[i][7] = static_cast<float>(sd * si);
			auto& model = MLPModel::instance();
			vec_temp[i].label = model.predict(features[i]);
			// vec_temp[i].label = vec_temp[i].self_idle_ratio;
		}
		// std::vector<float> scores;
		std::vector<K> to_get;
		for (auto& t : vec_temp)
		{
			// scores.push_back(t.label);
			if (t.label > 0.48)
			{
				to_get.push_back(t.key);
				t.label = -1;
			}
		}

		if (!to_get.empty())
		{
			std::lock_guard<std::mutex> lock(shard->mutex_);
			for (auto& key : to_get)
			{
				auto it = shard->cache_map.find(key);
				if (it != shard->cache_map.end())
				{
					shard->cache_list.splice(shard->cache_list.begin(),
						shard->cache_list, it->second);
					it->second->second->last_access_time_ = now();
					it->second->second->access_count_++;
				}
			}
		}

		std::sort(scores.begin(), scores.end());
		int num = scores.size();
		if (num > 0)
		{
			std::cout << "=== 分数分布 ===" << std::endl;
			std::cout << "10%分位: " << scores[num * 0.1] << std::endl;
			std::cout << "50%分位: " << scores[num * 0.5] << std::endl;
			std::cout << "90%分位: " << scores[num * 0.9] << std::endl;
			std::cout << "最小: " << scores[0] << ", 最大: " << scores[num - 1] << std::endl;
		}

		vec_temp.erase(std::remove_if(vec_temp.begin(), vec_temp.end(),
			[](const temp& node)
			{ return node.label < 0; }),
			vec_temp.end());

		size_t del_cnt = vec_temp.size() * 0;
		if (del_cnt == 0)
			shard->pending_ = false;
		return;

		std::nth_element(vec_temp.begin(), vec_temp.begin() + del_cnt, vec_temp.end(),
			[](const temp& a, const temp& b)
			{
				return a.label < b.label;
			});
		{
			std::lock_guard<std::mutex> lock(shard->mutex_);
			for (size_t i = 0; i < del_cnt; ++i)
			{
				auto it = shard->cache_map.find(vec_temp[i].key);
				if (it == shard->cache_map.end())
				{
					// std::cout << key << " cache is not exist" << std::endl;
					continue;
				}
				shard->cache_list.erase(it->second);
				shard->cache_map.erase(it);
				// std::cout << key << " cache is deleted" << std::endl;
			}
		}
		shard->pending_ = false; // 处理完成，重置 pending 状态
	}*/
};