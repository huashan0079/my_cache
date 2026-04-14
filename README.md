# cache_project
2026/4/14 添加了一个实验性缓存结构,此文件名为map.h,需要concurrentqueue库，需要头文件“concurrentqueue.h”，来源https://github.com/cameron314/concurrentqueue。
要求C++ 20版本
# 缓存系统设计文档 (my_cache)

## 1. 项目概述

`my_cache` 是一个高性能、并发安全的通用缓存库，实现了基于**热度传播模型**的自动淘汰策略。核心设计目标：

- 高并发读写（分片哈希表 + 细粒度锁）
- 自适应淘汰冷数据（热度衰减 + 桶扫描）
- 低延迟访问（无全局锁，后台线程异步维护热度）
- 支持任意键值类型（模板化设计）

---

## 2. 数学结构建模

### 2.1 热度定义

每个缓存节点 `node` 维护两个状态变量：

- `hotness`：当前热度值（初始 `1.0`）
- `percent`：自热占比（初始 `1.0`）

### 2.2 热度传播模型（类似 PageRank）

当访问节点 `n` 时，记录上一次访问的节点 `prev`（全局 `last_read`），调用 `n->add_predecessor(prev)`：

- 若 `prev == n`（连续访问同一节点）：
  ```
  self = percent * hotness
  self = self * 0.995 + 1
  hotness = (1 - percent) * hotness + self
  percent = self / hotness
  ```
- 否则，将 `prev` 加入 `n` 的前驱列表（最多 10 个，有效前驱最多保留 5 个）。

### 2.3 热度更新算法

后台线程 `Worker` 周期性扫描节点，调用 `update_hotness()`：

1. 从 `prevs` 中取最多 5 个有效前驱，累加其热度：
   ```
   hot = Σ (predecessor.hotness) * 0.3
   ```
2. 计算自身贡献：
   ```
   self = percent * hotness
   ```
3. 更新热度：
   ```
   hotness = self + hot
   hotness = min(hotness, 1023.0f)   // 上限截断
   if hotness == 0: hotness = 1
   ```
4. 更新占比：
   ```
   percent = self / hotness
   ```

### 2.4 热度衰减

每轮扫描后，对所有节点的 `hotness` 乘以 `0.9`（指数衰减）。

### 2.5 热度 → 桶 ID 映射

桶 ID 范围 `0~99`，由 `hotness` 通过分段对数函数计算：

```
bucket_id = floor( log2(hotness + 1) * 10 )
```

对应查表实现，保证 O(1) 计算。

### 2.6 淘汰策略

当缓存总大小超过容量 `1.1` 倍时，从桶 `0` 到 `99` 依次取出节点，删除直到容量恢复。**桶 ID 越小 → 热度越低 → 优先淘汰**。

---

## 3. 模块用途与作者意图

| 模块 | 用途 | 作者意图 |
|------|------|----------|
| `ShardedHashMap<K,V>` | 分片哈希表，存储 `key → shared_ptr<node>` | 降低锁竞争，支持高并发读写 |
| `Bucket<V>` | 管理同一热度桶内的节点指针 | 快速批量扫描和移动节点 |
| `Worker<K,V>` | 后台线程：处理新节点、更新热度、执行淘汰 | 异步维护，避免阻塞主线程 |
| `node<K,V>` | 缓存条目，存储键值、热度、前驱关系 | 实现热度传播模型 |
| `Map_Cache<K,V>` | 核心缓存门面，整合 `ShardedHashMap` + `Worker` | 对外提供 `put` / `get_shared` 接口 |
| `ShardedCache<K,V>` | 对 `Map_Cache` 进一步分片（容量 >10000 时自动分片） | 突破单实例容量上限，提升扩展性 |

**作者意图总结**：  
设计一个**热度感知**的缓存系统，避免传统 LRU 的“偶发访问污染”和 LFU 的“旧热点僵化”问题。通过前驱传播和指数衰减，使热度能快速响应访问模式变化，同时利用分片和异步维护实现高吞吐。

---

## 4. 使用方法

### 4.1 包含头文件
```cpp
#include "map.h"   // 假设头文件名为 map.h
using namespace my_cache;
```

### 4.2 创建缓存实例

```cpp
// 总容量 5000 个条目（自动选择单分片）
ShardedCache<std::string, int> cache(5000);

// 总容量 20000 个条目（自动分成 2 个分片，每片 10000）
ShardedCache<int, double> cache2(20000);
```

### 4.3 写入缓存

```cpp
std::string key = "answer";
int value = 42;
cache.put(key, std::move(value));
```

### 4.4 读取缓存

```cpp
auto node_ptr = cache.get_shared(key);
if (node_ptr) {
    std::cout << "Value: " << node_ptr->value << std::endl;
} else {
    std::cout << "Not found" << std::endl;
}
```

### 4.5 注意事项

- `value` 类型需要支持移动语义（`std::move`）。
- 读取返回 `shared_ptr<node<K,V>>`，可通过 `node->value` 获取值。
- 禁止直接修改 `node->hotness` 或 `node->percent`，应由系统自动维护。
- 缓存满时，`put` 可能会短暂自旋等待淘汰完成（上限约 10000 次自旋），不会死锁。

---

## 5. 主要函数说明

### 5.1 `ShardedHashMap`

| 函数 | 说明 |
|------|------|
| `V get(const K& key)` | 返回键对应的 `shared_ptr<node>`（可能为空） |
| `void put(const K& key, V&& value)` | 插入或更新节点（覆盖旧值） |
| `bool erase(const K& key)` | 删除键，返回是否成功 |
| `bool insert_if_not_exists(const K& key, V&& value)` | 仅当键不存在时插入，返回是否插入成功 |

### 5.2 `Bucket`

| 函数 | 说明 |
|------|------|
| `void put(V& value)` | 向桶内插入节点指针（若已存在则忽略） |
| `bool erase(const V& value)` | 从桶内移除节点 |
| `void getAll(std::vector<V>& vec)` | 获取桶内所有节点指针（拷贝到 vec） |
| `void clear()` | 清空桶 |
| `size_t size()` | 桶内节点数量 |

### 5.3 `Map_Cache`

| 函数 | 说明 |
|------|------|
| `void put(const K& k, V&& v)` | 插入或更新缓存条目 |
| `const std::shared_ptr<node<K,V>> get_shared(const K& k)` | 读取缓存，同时触发热度传播 |

### 5.4 `ShardedCache`

| 函数 | 说明 |
|------|------|
| `void put(const K& key, V&& value)` | 写入缓存（自动路由到正确分片） |
| `const std::shared_ptr<node<K,V>> get_shared(const K& key)` | 读取缓存 |
| `int get_shard_count()` | 获取分片数量 |
| `int get_total_capacity()` | 获取总容量（向上取整后） |

---

## 6. 内部工作机制

```
用户线程                    后台线程 (Worker)
    │                            │
    │ put(key, value)            │
    ├──► 检查是否已存在           │
    │    └─ 若不存在则创建 node   │
    │    └─ 插入 ShardedHashMap   │
    │    └─ 提交 node 到队列 ────►│ 1. 从队列取新 node
    │                            │ 2. 计算初始 bucket_id
    │ get_shared(key)            │ 3. 放入对应桶
    ├──► 从 HashMap 取 node      │
    │    └─ 记录 last_read        │
    │    └─ 调用 add_predecessor  │
    │                            │ 每 8ms 循环：
    │                            │   - 扫描部分桶
    │                            │   - 调用 update_hotness()
    │                            │   - hotness *= 0.9
    │                            │   - 重新计算 bucket_id
    │                            │   - 移动桶
    │                            │   - 若总大小超过容量 1.1 倍
    │                            │     └─ 从低桶到高桶淘汰
```

---

## 7. 性能建议

- 单分片容量建议 ≤ 10000，超过后自动分片。
- 适合读多写少、访问模式频繁变化的场景。
- 热度传播的开销与访问序列相关，连续访问同一节点时额外计算较多。
- 后台线程默认每 8ms 执行一轮，可通过修改 `Worker::loop` 中的 `sleep_for` 调整。
  
## 8.当前已知问题

-没有解决原子变量乒乓
-权限管理简易，没有做好访问安全
-各种参数没有变量化，依赖硬编码
-可能需要内存池，但目前测试时没有解决内存池与智能指针管理内存的冲突问题
