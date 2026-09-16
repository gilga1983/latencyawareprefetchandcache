#include "../src/a2_map.hpp"
#include <cassert>
using namespace lapsim;
int main(){
 A2Map m(100,10,10);
 // With only demand reuse, all-cache should remain best.
 for(int i=0;i<20;++i){m.before_demand(1,1);if(m.due())assert(m.choose(100)==100);}
 // Prediction stack records a residual opportunity and controller remains valid.
 A2Map p(100,10,4);p.observe_prediction(7,1);p.before_demand(7,1);p.before_demand(8,3);
 if(p.due()){auto c=p.choose(100);assert(c<=100);assert(p.epochs()==1);}
}
