#include "sim.hpp"
#include "oracle_trace.hpp"
#include "prefetchers.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace lapsim;

namespace {
struct Args {
    std::string trace, name = "trace", prefetcher = "none";
    Bytes capacity = 256ULL * 1024 * 1024;
    std::vector<Time> latencies_us{0,1000,5000,10000,20000};
    std::uint64_t limit=0; Time ticks_per_second=1000000; bool spread_within_second=true;
};
std::vector<Time> parse_latencies(const std::string& text){std::vector<Time> out;std::stringstream ss(text);std::string item;while(std::getline(ss,item,',')){if(item.empty())continue;long long v=std::stoll(item);if(v<0)throw std::invalid_argument("latencies must be non-negative");out.push_back(static_cast<Time>(v));}if(out.empty())throw std::invalid_argument("at least one latency is required");if(std::find(out.begin(),out.end(),0)==out.end())out.insert(out.begin(),0);return out;}
Args parse_args(int argc,char** argv){Args a;for(int i=1;i<argc;++i){std::string s=argv[i];auto need=[&](){if(i+1>=argc)throw std::invalid_argument("missing value for "+s);return std::string(argv[++i]);};if(s=="--trace")a.trace=need();else if(s=="--name")a.name=need();else if(s=="--prefetcher")a.prefetcher=need();else if(s=="--capacity")a.capacity=std::stoull(need());else if(s=="--latencies-us")a.latencies_us=parse_latencies(need());else if(s=="--limit")a.limit=std::stoull(need());else if(s=="--ticks-per-second")a.ticks_per_second=std::stoll(need());else if(s=="--time-model"){auto v=need();if(v=="spread")a.spread_within_second=true;else if(v=="raw")a.spread_within_second=false;else throw std::invalid_argument("time model must be spread or raw");}else throw std::invalid_argument("unknown argument: "+s);}if(a.trace.empty())throw std::invalid_argument("usage: trace_bench --trace FILE [--prefetcher none|obl|stride|pg]");if(!a.capacity||a.ticks_per_second<=0)throw std::invalid_argument("invalid capacity/tick rate");return a;}
struct Scenario{Time latency_us;Simulator sim;explicit Scenario(Time l,Bytes c):latency_us(l),sim(Simulator::fixed_lru(c,l)){}};
void json_string(std::ostream& out,const std::string&s){out<<'"';for(char c:s){if(c=='\\')out<<"\\\\";else if(c=='"')out<<"\\\"";else if(c=='\n')out<<"\\n";else out<<c;}out<<'"';}
}

int main(int argc,char**argv){try{
 const Args a=parse_args(argc,argv); auto pf=make_prefetcher(a.prefetcher);
 std::ifstream file;std::istream*in=nullptr;if(a.trace=="-"){std::cin.sync_with_stdio(false);in=&std::cin;}else{file.open(a.trace,std::ios::binary);if(!file)throw std::runtime_error("cannot open trace: "+a.trace);in=&file;}
 ByteLru instant(a.capacity);std::vector<Scenario> scenarios;for(Time l:a.latencies_us)scenarios.emplace_back(l,a.capacity);
 std::uint64_t requests=0,demand_bytes=0,instant_hits=0,instant_miss_bytes=0,bucket_count=0,max_bucket_requests=0,predictions=0;
 bool have_first_clock=false;std::uint32_t first_clock=0,last_clock=0;Time last_arrival=0;std::vector<OracleRecord> bucket;bucket.reserve(4096);
 auto process_bucket=[&](const std::vector<OracleRecord>& records){if(records.empty())return;++bucket_count;max_bucket_requests=std::max<std::uint64_t>(max_bucket_requests,records.size());auto clock=records.front().clock_time;if(!have_first_clock){first_clock=clock;have_first_clock=true;}if(clock<first_clock)throw std::runtime_error("trace timestamp precedes first timestamp");std::uint64_t sec_delta=clock-first_clock;if(sec_delta>static_cast<std::uint64_t>(std::numeric_limits<Time>::max()/a.ticks_per_second))throw std::overflow_error("trace time overflows simulator Time");Time base=static_cast<Time>(sec_delta)*a.ticks_per_second;std::size_t m=records.size();
  for(std::size_t k=0;k<m;++k){const auto&rec=records[k];if(rec.size==0||static_cast<Bytes>(rec.size)>a.capacity)throw std::runtime_error("requires 0 < object_size <= cache capacity");Time arrival=base;if(a.spread_within_second){__int128 numerator=static_cast<__int128>(k+1)*a.ticks_per_second;arrival+=static_cast<Time>(numerator/static_cast<__int128>(m+1));}if(arrival<last_arrival)throw std::runtime_error("reconstructed time moved backwards");last_arrival=arrival;++requests;demand_bytes+=rec.size;
   bool hit=instant.on_demand(rec.object,rec.size,arrival);if(hit)++instant_hits;else{instant_miss_bytes+=rec.size;instant.on_fill(rec.object,rec.size,arrival,FetchOrigin::Demand);}
   Request req{arrival,rec.object,rec.size};for(auto&s:scenarios)s.sim.demand(req);
   auto preds=pf->on_demand(req);predictions+=preds.size();for(const auto&p:preds)for(auto&s:scenarios)s.sim.prefetch(arrival,p.object,p.size);
  }last_clock=clock;};
 OracleRecord rec;bool have_bucket=false;std::uint32_t bucket_clock=0;while((!a.limit||requests+bucket.size()<a.limit)&&read_oracle_record(*in,rec)){if(!have_bucket){bucket_clock=rec.clock_time;have_bucket=true;}if(rec.clock_time<bucket_clock)throw std::runtime_error("timestamps not nondecreasing");if(rec.clock_time!=bucket_clock){process_bucket(bucket);bucket.clear();if(a.limit&&requests>=a.limit)break;bucket_clock=rec.clock_time;}if(!a.limit||requests+bucket.size()<a.limit)bucket.push_back(rec);}process_bucket(bucket);if(!requests)throw std::runtime_error("trace contained no requests");
 double reference_hr=static_cast<double>(instant_hits)/requests;std::cout<<std::setprecision(15)<<"{\"trace\":";json_string(std::cout,a.name);std::cout<<",\"prefetcher\":";json_string(std::cout,pf->name());std::cout<<",\"requests\":"<<requests<<",\"predictions\":"<<predictions<<",\"demand_bytes\":"<<demand_bytes<<",\"capacity_bytes\":"<<a.capacity<<",\"time_model\":";json_string(std::cout,a.spread_within_second?"spread":"raw");std::cout<<",\"ticks_per_second\":"<<a.ticks_per_second<<",\"coarse_buckets\":"<<bucket_count<<",\"max_requests_in_second\":"<<max_bucket_requests<<",\"first_clock\":"<<first_clock<<",\"last_clock\":"<<last_clock<<",\"instant_lru_hits\":"<<instant_hits<<",\"instant_lru_hit_ratio\":"<<reference_hr<<",\"instant_lru_miss_bytes\":"<<instant_miss_bytes<<",\"scenarios\":[";
 bool first=true;for(const auto&s:scenarios){const auto&m=s.sim.metrics();if(!first)std::cout<<',';first=false;double n=static_cast<double>(m.demands),resident=n?m.resident_hits/n:0,delayed=n?m.prefetch_delayed_hits/n:0,coalesced=n?m.demand_coalesced_hits/n:0,no_new=n?(m.resident_hits+m.prefetch_delayed_hits+m.demand_coalesced_hits)/n:0;std::cout<<"{\"latency_us\":"<<s.latency_us<<",\"resident_hits\":"<<m.resident_hits<<",\"resident_hit_ratio\":"<<resident<<",\"prefetch_delayed_hits\":"<<m.prefetch_delayed_hits<<",\"prefetch_delayed_ratio\":"<<delayed<<",\"demand_coalesced_hits\":"<<m.demand_coalesced_hits<<",\"demand_coalesced_ratio\":"<<coalesced<<",\"new_misses\":"<<m.new_misses<<",\"served_without_new_io_ratio\":"<<no_new<<",\"backend_fetches\":"<<m.backend_fetches<<",\"backend_bytes\":"<<m.backend_bytes<<",\"average_access_time_us\":"<<m.average_access_time()<<",\"prefetch_issued\":"<<m.prefetch_issued<<",\"prefetch_resident_redundant\":"<<m.prefetch_resident_redundant<<",\"prefetch_inflight_redundant\":"<<m.prefetch_inflight_redundant<<",\"prefetch_oversize_suppressed\":"<<m.prefetch_oversize_suppressed;if(s.latency_us>0)std::cout<<",\"aat_fraction_of_backend_latency\":"<<(m.average_access_time()/static_cast<double>(s.latency_us));std::cout<<"}";}std::cout<<"]}\n";
 if(a.prefetcher=="none"){auto z=std::find_if(scenarios.begin(),scenarios.end(),[](const Scenario&s){return s.latency_us==0;});if(z==scenarios.end())throw std::logic_error("zero-latency scenario missing");const auto&m=z->sim.metrics();if(m.resident_hits!=instant_hits||m.new_misses!=requests-instant_hits||m.demand_coalesced_hits!=0)throw std::runtime_error("zero-latency simulator does not reproduce immediate byte-LRU");}
 return 0;}catch(const std::exception&e){std::cerr<<"trace_bench: "<<e.what()<<'\n';return 2;}}
