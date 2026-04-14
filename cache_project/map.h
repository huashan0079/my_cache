#pragma once
#include <atomic>
#include <memory>
#include <unordered_map>
#include <shared_mutex>
#include <thread>
#include <list>
#include <vector>
#include <cmath>
#include<queue>
#include "concurrentqueue.h"
    namespace my_cache {

       
        template<typename K, typename V>
        class ShardedHashMap {
        public:
            static constexpr size_t SHARDS = 1024; // 32分片，性能极佳
            std::atomic<uint64_t> size{ 0 };
            struct Shard {
                std::unordered_map<K, V> map;
                mutable std::shared_mutex mutex;
            };

            std::vector<Shard> shards_{ SHARDS };

            Shard& get_shard(const K& key) {
                size_t idx = std::hash<K>{}(key) % SHARDS;
                return shards_[idx];
            }

            // 读
            
            V get(const K& key) const {
                auto& shard = const_cast<ShardedHashMap*>(this)->get_shard(key);
                std::shared_lock lock(shard.mutex);
                auto it = shard.map.find(key);
                return (it != shard.map.end()) ? it->second : nullptr;
            }

            // 插入 or 更新
            void put(const K& key, V&& value) {
                auto& shard = get_shard(key);
                size++;
                std::unique_lock lock(shard.mutex);
                shard.map[key] = std::move(value);
            }

            // 删除
            bool erase(const K& key) {
                auto& shard = get_shard(key);
                std::unique_lock lock(shard.mutex);
                size_t count=shard.map.erase(key);
                if (count > 0) {
                    size--;
                    //std::cout << "erase success" << std::endl;
                    return true;
                }
                else {
                    std::cout<<"erase error"<<std::endl;
                    return false;
                }
            }

           

            bool insert_if_not_exists(const K& key, V&& value);
        };

        template<typename V>
        class Bucket {
        private:
            std::mutex mutex_;
            std::list<V> cache_list;
            std::unordered_map<V, typename std::list<V>::iterator> cache_map;

        public:
            // 插入或更新节点
            void put(V& value) {
                //std::lock_guard lock(mutex_);
                auto it = cache_map.find(value);
                if (it != cache_map.end()) {
                    return;
                }
                else {
                    cache_list.push_back(value);
                    cache_map[value] = --cache_list.end();
                }
            }

            bool erase(const V& value) {
                //std::lock_guard lock(mutex_);
                auto it = cache_map.find(value);
                if (it != cache_map.end()) {
                    cache_list.erase(it->second);
                    cache_map.erase(it);
                    return true;
                }
                return false;
            }

            void getAll(std::vector<V>& vec) {
                //std::lock_guard lock(mutex_);
                vec.clear();
                vec.reserve(cache_list.size());
                for (auto& v : cache_list) {
                    vec.push_back(v);
                }
            }

            void clear() {
                //std::lock_guard lock(mutex_);
                cache_list.clear();
                cache_map.clear();
            }
            // 获取当前桶的大小
            size_t size() {
                //std::lock_guard lock(mutex_);
                return cache_map.size();
            }

            bool empty() {
                //std::lock_guard lock(mutex_);
                return cache_list.empty();
            }
        };
        
       

        // 前向声明
        template<typename K, typename V>
        class node;

        template<typename K, typename V>
        class Worker;

        template<typename K, typename V>
        class Map_Cache;

        // ==================== Worker 类 ====================
        template<typename K, typename V>
        class Worker {
            static constexpr int MAX_H = 1000;
            static constexpr int PRECISION = 10;         // 精度 0.1
            static constexpr int TABLE_SIZE = MAX_H * PRECISION + 1;  // 10001

             uint8_t bucket_table[TABLE_SIZE];
             bool table_initialized;

            void init_table() {
                for (int i = 0; i < TABLE_SIZE; i++) {
                    float h = (float)i / PRECISION;
                    // 关键：必须用 log2f 计算，保证和原函数一致
                    size_t logv = (size_t)(log2f(h + 1.0f) * 10);
                    bucket_table[i] = (uint8_t)std::min((size_t)99, logv);
                }
                table_initialized = true;
            }
        public:
            std::thread thread;
            std::mutex mutex;
            std::atomic<bool> running{ true };
            std::vector<Bucket<node<K, V>*>> buckets{100};
            int capacity;
            Map_Cache<K, V>* map;
            moodycamel::ConcurrentQueue<node<K, V>*> submit_queue;
            //std::mutex submit_mutex;
            Worker(int cap, Map_Cache<K, V>* map_);
            ~Worker();
            void submit(node<K, V>* n);
            
        private:
            void loop();
            size_t cal_bucket_id(node<K, V>* n);
        };

        // ==================== node 类 ====================
        template<typename K, typename V>
        class node {
        public:
            std::atomic<float> hotness{ 1.0f };
            int bucket_id{ -1 };
            std::atomic<bool> updated{ false };
            float percent;
            std::list<std::weak_ptr<node<K, V>>> prevs;
            K key;
            Worker<K, V>* worker;
            std::mutex mutex_;
            
            V value;
            
            //bool in_bucket{ false };
            node() : hotness(0.0f), bucket_id(-1), updated(false),
                percent(0.0f), worker(nullptr) {
            }
            node(const K& k, V&& v, Worker<K, V>* worker_);
            ~node();
            void update(V&& v);
            void add_predecessor(std::shared_ptr<node<K, V>> prev);
            void update_hotness();
        };

        // ==================== Map_Cache 类 ====================
        template<typename K, typename V>
        class Map_Cache {
        public:
            std::atomic<int> current_size{ 0 };
            int capacity;
            
            std::unique_ptr<Worker<K, V>> worker;
            std::atomic<std::shared_ptr<node<K, V>>> last_read{ nullptr };
            ShardedHashMap<K, std::shared_ptr<node<K, V>>> hashmap;
            
            Map_Cache(int cap = 100);
            ~Map_Cache();

            void put(const K& k, V&& v);
            const std::shared_ptr<node<K, V>> get_shared(const K& k);

            // 供 Worker 调用的接口
            
        };



            template<typename K, typename V>
            class ShardedCache {
            public:
                // 单片容量上限（甜点容量）
                static constexpr int MAX_SHARD_CAPACITY = 10000;

                // 构造函数：根据总容量自动计算分片数
                // 10000 以下：单分片，容量 = total_capacity（按需）
                // 10000 以上：多分片，每片 10000，向上取整
                ShardedCache(int total_capacity) {
                    if (total_capacity <= 0) {
                        total_capacity = 100;  // 默认最小容量
                    }

                    if (total_capacity <= MAX_SHARD_CAPACITY) {
                        // 10000 以下：单分片，按需分配
                        shard_count_ = 1;
                        shards_.emplace_back(std::make_unique<Map_Cache<K, V>>(total_capacity));
                        total_capacity_ = total_capacity;
                    }
                    else {
                        // 10000 以上：多分片，每片 10000
                        shard_count_ = (total_capacity + MAX_SHARD_CAPACITY - 1) / MAX_SHARD_CAPACITY;
                        for (int i = 0; i < shard_count_; ++i) {
                            shards_.emplace_back(std::make_unique<Map_Cache<K, V>>(MAX_SHARD_CAPACITY));
                        }
                        total_capacity_ = shard_count_ * MAX_SHARD_CAPACITY;
                    }

                    std::cout << "ShardedCache created: " << shard_count_
                        << " shards, total capacity: " << total_capacity_ << std::endl;
                }

                // 根据 key 计算分片索引
                size_t get_shard_idx(const K& key) const {
                    return std::hash<K>{}(key) % shard_count_;
                }

                // 写入
                void put(const K& key, V&& value) {
                    shards_[get_shard_idx(key)]->put(key, std::forward<V>(value));
                }

                // 读取
                const std::shared_ptr<node<K, V>> get_shared(const K& key) {
                    return shards_[get_shard_idx(key)]->get_shared(key);
                }

                // 获取总分片数
                int get_shard_count() const {
                    return shard_count_;
                }

                // 获取总容量
                int get_total_capacity() const {
                    return total_capacity_;
                }

            private:
                std::vector<std::unique_ptr<Map_Cache<K, V>>> shards_;
                int shard_count_;
                int total_capacity_;
            };



        template<typename K, typename V>
        bool ShardedHashMap<K,V>::insert_if_not_exists(const K& key, V&& value) {
            auto& shard = get_shard(key);
            std::unique_lock lock(shard.mutex);
            auto [it, inserted] = shard.map.try_emplace(key, std::forward<V>(value));
            if (inserted) {
                size++;
            }
            return inserted;
        }


        // ==================== Worker 实现 ====================
        template<typename K, typename V>
        Worker<K, V>::Worker(int cap, Map_Cache<K, V>* map_)
            : capacity(cap), map(map_) {
            init_table();
            thread = std::thread([this]() { loop(); });
        }

        template<typename K, typename V>
        Worker<K, V>::~Worker() {
            running = false;
            if (thread.joinable())
                thread.join();
        }

        template<typename K, typename V>
        void Worker<K, V>::submit(node<K, V>* n) {
            
            submit_queue.enqueue(n);
        }

        template<typename K, typename V>
        size_t Worker<K, V>::cal_bucket_id(node<K, V>* n) {
            if (!table_initialized) init_table();

            float h = n->hotness;
          
            if (h < 0.0f) h = 0.0f;
            if (h > (float)MAX_H) h = (float)MAX_H;

            int idx = (int)(h * PRECISION);
            return bucket_table[idx];
        }

        template<typename K, typename V>
        void Worker<K, V>::loop() {
            using namespace std::chrono;
            size_t total_rounds = 0;
            size_t total_new_nodes = 0;
            size_t total_stock_nodes = 0;
            uint64_t time_fetch_queue = 0;
            uint64_t time_process_new = 0;
            uint64_t time_hotness_update = 0;
            uint64_t time_evict = 0;

            int scan_offset = 0;
            const int BUCKETS_PER_ROUND = 10;
            const int STEP = 10;  // 100 / 10 = 10
            while (running) {
                
                std::this_thread::sleep_for(std::chrono::milliseconds(8));
                auto t1 = steady_clock::now();
                // 1. 衰减热度 + 换桶
                std::vector<node<K, V>*> to_process;
                    // 2. 只取 count 个，取完立即停止
                    node<K, V>* n;
                   
                    while (submit_queue.try_dequeue(n)) {
                        to_process.push_back(n);
                    }
                
                // 批量处理
                    auto t2 = steady_clock::now();
                    time_fetch_queue += duration_cast<microseconds>(t2 - t1).count();

                    // 2. 处理新节点
                    auto t3 = steady_clock::now();
                for (auto& n : to_process) {
                    if (n->bucket_id != -1) {
                        std::cerr << "Error: node already has a bucket_id" << std::endl;
                    }
                    //n->hotness = n->hotness * 0.9;
                    size_t new_bucket_id = cal_bucket_id(n);
                    n->bucket_id = new_bucket_id;
                    buckets[new_bucket_id].put(n);
                }
                auto t4 = steady_clock::now();
                time_process_new += duration_cast<microseconds>(t4 - t3).count();

                // 3. 热度更新
                auto t5 = steady_clock::now();
                int total_collected = 0;
                std::vector<node<K, V>*> to_remove;
                
                for (int count = 0; count <BUCKETS_PER_ROUND ; count++) {
                    int i = scan_offset + count * STEP;
                    
                    {

                        {
                            buckets[i].getAll(to_remove);
                            total_collected += buckets[i].size();
                            for (auto& n : to_remove) {
                                n->update_hotness();
                                n->hotness = n->hotness * 0.9;
                                int new_bucket_id = cal_bucket_id(n);
                                if (new_bucket_id != i) {
                                    buckets[i].erase(n);
                                    buckets[new_bucket_id].put(n);
                                    n->bucket_id = new_bucket_id;
                                }
                            }
                        }

                    }
                }
                scan_offset = (scan_offset + 1) % STEP;
                auto t6 = steady_clock::now();
                time_hotness_update += duration_cast<microseconds>(t6 - t5).count();

                // 4. 淘汰
                auto t7 = steady_clock::now();
                 
                if (map->hashmap.size > map->capacity*1.1 ) {
                    int to_evict = map->hashmap.size - map->capacity;
                    std::vector<node<K, V>*> to_erase;
                    std::vector<K> keys_to_evict;
                    for (int i = 0; i < 100 && keys_to_evict.size() < to_evict; i++) {
                       buckets[i].getAll(to_erase);
                       if (to_evict - keys_to_evict.size() < to_erase.size()) {
                           for (auto& n : to_erase) {
                               keys_to_evict.push_back(n->key);
                               //n->updated=false;
                               //n->bucket_id=-1;
                               buckets[i].erase(n);
                               if (keys_to_evict.size() >= to_evict) {
                                   break;
                               }
                           }
                       }
                       else {
                           for (auto& n : to_erase) {
                               keys_to_evict.push_back(n->key);
                           }
                           buckets[i].clear();
                       }

                       
                    }
                    if (!keys_to_evict.empty()) {
                        for (auto& key : keys_to_evict) {
                            map->hashmap.erase(key);
                        }
                    }
                }
                auto t8 = steady_clock::now();
                time_evict += duration_cast<microseconds>(t8 - t7).count();
                
            }
            
            std::cout << "\n=== Worker  lifetime 耗时统计 ===\n";
            std::cout << "总轮次: " << total_rounds << "\n";
            std::cout << "总新节点: " << total_new_nodes << "\n";
            std::cout << "总存量节点处理: " << total_stock_nodes << "\n\n";

            std::cout << "1. 取出队列:   " << time_fetch_queue / 1000 << " ms\n";
            std::cout << "2. 处理新节点: " << time_process_new / 1000 << " ms\n";
            std::cout << "3. 热度更新:   " << time_hotness_update / 1000 << " ms\n";
            std::cout << "4. 淘汰:       " << time_evict / 1000 << " ms\n";

            uint64_t total = time_fetch_queue + time_process_new +
                time_hotness_update + time_evict;
            if (total > 0) {
                std::cout << "\n占比:\n";
                std::cout << "  取出队列:   " << time_fetch_queue * 100 / total << "%\n";
                std::cout << "  处理新节点: " << time_process_new * 100 / total << "%\n";
                std::cout << "  热度更新:   " << time_hotness_update * 100 / total << "%\n";
                std::cout << "  淘汰:       " << time_evict * 100 / total << "%\n";
            }
        }

        // ==================== node 实现 ====================
        template<typename K, typename V>
        node<K, V>::node(const K& k, V&& v, Worker<K, V>* worker_)
            : key(k), value(std::forward<V>(v)), percent(1.0f), worker(worker_)
            
        {
        }
        template<typename K, typename V>
        node<K, V>::~node() {
            
            
        }
        template<typename K, typename V>
        void node<K, V>::update(V&& v) {
            
            std::lock_guard lock(mutex_);
           
            value = std::forward<V>(v);
        }

        template<typename K, typename V>
        void node<K, V>::add_predecessor(std::shared_ptr<node<K, V>> prev) {
            //std::cout << "add_predecessor: this=" << this << " key=" << key << std::endl;
            //std::cout << "add_predecessor: prev=" << prev.get() << " prev_key=" << prev->key << std::endl;
            std::lock_guard lock(mutex_);
            if (prev.get() == this) {
                float self = percent * hotness;
                self = self *0.995+1;
                hotness = (1 - percent) * hotness + self;
                if (hotness > 0) {
                    percent = self / hotness;
                }
                else {
                    percent = 1.0f;
                }
            }
            else {
                prevs.emplace_front(prev);
                if(prevs.size()>10) prevs.pop_back();
            }
            updated = true;
            
        }

        template<typename K, typename V>
        void node<K, V>::update_hotness() {
            if (!updated) return;
            std::lock_guard lock(mutex_);
            updated = false;

            // 清理无效 + 收集有效（最多5个）
            float hot = 0;
            int valid_count = 0;

            for (auto it = prevs.begin(); it != prevs.end(); ) {
                if (auto sp = it->lock()) {
                    if (valid_count < 5) {
                        hot += sp->hotness.load(std::memory_order_relaxed);
                        valid_count++;
                    }
                    ++it;
                }
                else {
                    it = prevs.erase(it);  // 删除无效
                }
            }

            // 删除多余的（超过5个的有效节点）
            while (prevs.size() > 5) {
                prevs.pop_back();
            }

            // 更新热度
            hotness = std::min((float)hotness, 1023.0f);
            float self = percent * hotness;
            hotness = self  + hot * 0.3f;
            hotness=std::min((float)hotness, 1023.0f);
            if(hotness==0) hotness=1;
            percent = self / hotness;
        }
        // ==================== Map_Cache 实现 ====================
        template<typename K, typename V>
        Map_Cache<K, V>::Map_Cache(int cap) : capacity(cap) {
            
            worker = std::make_unique<Worker<K, V>>(cap, this);
        }

        template<typename K, typename V>
        Map_Cache<K, V>::~Map_Cache() {
          
			worker.reset();
        }

        template<typename K, typename V>
        void Map_Cache<K, V>::put(const K& k, V&& v) {
            
            
            int spin_count = 0;
            while ( hashmap.size >= capacity*1.55) {
                spin_count++;

                if (spin_count < 10) {
                    // 前10次：纯自旋（适合极短等待）
                    _mm_pause();  // x86 PAUSE指令，减少功耗
                }
                else if (spin_count < 100) {
                    // 10-100次：让出CPU
                    std::this_thread::yield();
                }
                else {
                    // 超过100次：真正睡眠
                    std::this_thread::sleep_for(std::chrono::microseconds(100 * (spin_count - 100)));
                }

                // 防止死循环（理论上不会发生）
                if (spin_count > 10000) {
                    // 超时，强制写入（可能OOM，但比死锁好）
                    std::cerr << "Warning: cache overload timeout, force write\n";
                    break;
                }
            }
            auto n = hashmap.get(k);  // 分片接口直接返回 node
            if (n) {                   // 不为空就是找到
                n->update(std::forward<V>(v));
                return;
            }
            auto new_node = std::shared_ptr<node<K, V>>(new node<K, V>(k, std::forward<V>(v), worker.get()));
            //auto new_node = allocate_node(k, std::forward<V>(v));

            auto temp=new_node;
            if (hashmap.insert_if_not_exists(k, std::move(temp))) {
                current_size++;
                new_node->updated = false;
                new_node->worker->submit(new_node.get());
                
            }
            else {
                // 插入失败，说明有其他线程抢先了
                auto existing = hashmap.get(k);
                existing->update(std::forward<V>(v));
            }
            
        }

        template<typename K, typename V>
        const std::shared_ptr<node<K, V>> Map_Cache<K, V>::get_shared(const K& k) {
            std::shared_ptr<node<K, V>> n;
            {
                n = hashmap.get(k);
                if (!n) return nullptr;
            }
            auto prev = last_read.exchange(n, std::memory_order_acq_rel);
            if (prev) {
                n->add_predecessor(prev);
            }
            return n;
        }

    } // namespace my_cache