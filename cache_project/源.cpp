#include <iostream>
#include "cache.h"

using namespace std;
using namespace std::chrono;
using namespace my_cache;
int main() {
    Cache<int, int> cache(CacheType::LRU, 10);

    return 0;
}