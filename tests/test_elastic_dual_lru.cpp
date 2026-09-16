#include "../src/elastic_dual_lru.hpp"
#include <cassert>
using lapsim::ElasticDualLru;
int main(){
  ElasticDualLru x(100,60);
  x.demand_fill(1,40); x.demand_fill(2,40); // C borrows P slack
  assert(x.cache_bytes()==80 && x.prefetch_bytes()==0);
  assert(x.prefetch_fill(3,20));
  assert(x.used()==100 && x.in_prefetch(3));
  auto k=x.demand(3);
  assert(k==ElasticDualLru::DemandKind::PrefetchHit);
  assert(x.in_cache(3) && !x.in_prefetch(3));
  x.prefetch_fill(4,30); x.check();
  assert(x.used()<=100);
  for(unsigned i=1;i<10;i++) assert(!(x.in_cache(i)&&x.in_prefetch(i)));
  x.set_cache_quota(20); x.check();
  assert(x.used()<=100);
}
