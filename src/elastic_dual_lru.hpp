#pragma once
#include <algorithm>
#include <cstdint>
#include <list>
#include <stdexcept>
#include <unordered_map>

namespace lapsim {

// Policy core for A2. C and P are disjoint logical LRUs sharing one physical
// byte budget. Protected quotas define reclamation preference; either side may
// borrow slack from the other. A P demand consumes P and promotes into C.
class ElasticDualLru {
public:
    using ObjectId=std::uint64_t; using Bytes=std::uint64_t;
    enum class DemandKind { Miss, CacheHit, PrefetchHit };
    explicit ElasticDualLru(Bytes capacity, Bytes cache_quota)
        : capacity_(capacity) { set_cache_quota(cache_quota); }
    Bytes capacity()const{return capacity_;} Bytes cache_quota()const{return cq_;}
    Bytes prefetch_quota()const{return capacity_-cq_;} Bytes cache_bytes()const{return cb_;}
    Bytes prefetch_bytes()const{return pb_;} Bytes used()const{return cb_+pb_;}
    bool in_cache(ObjectId x)const{return c_.count(x);} bool in_prefetch(ObjectId x)const{return p_.count(x);}
    void set_cache_quota(Bytes q){cq_=std::min(q,capacity_); reclaim();}
    DemandKind demand(ObjectId x,Bytes size){
        auto ci=c_.find(x); if(ci!=c_.end()){touch(corder_,ci->second.pos);return DemandKind::CacheHit;}
        auto pi=p_.find(x); if(pi!=p_.end()){
            Bytes keep=std::max(size,pi->second.size); erase_p(pi); insert_c(x,keep); reclaim(); return DemandKind::PrefetchHit;
        }
        return DemandKind::Miss;
    }
    void demand_fill(ObjectId x,Bytes size){ if(size>capacity_)return; erase_any(x); insert_c(x,size); reclaim(); }
    bool prefetch_fill(ObjectId x,Bytes size){
        if(size>capacity_||in_cache(x))return false; auto pi=p_.find(x);
        if(pi!=p_.end()){ if(size>pi->second.size){pb_+=size-pi->second.size;pi->second.size=size;} touch(porder_,pi->second.pos);reclaim();return true;}
        insert_p(x,size); reclaim(); return in_prefetch(x);
    }
    void check()const{
        if(cb_+pb_>capacity_)throw std::logic_error("ElasticDualLru budget exceeded");
        for(auto const&kv:c_)if(p_.count(kv.first))throw std::logic_error("C/P overlap");
    }
private:
    struct E{Bytes size;std::list<ObjectId>::iterator pos;};
    static void touch(std::list<ObjectId>&o,std::list<ObjectId>::iterator &p){o.splice(o.end(),o,p);p=std::prev(o.end());}
    void erase_c(typename std::unordered_map<ObjectId,E>::iterator i){cb_-=i->second.size;corder_.erase(i->second.pos);c_.erase(i);}
    void erase_p(typename std::unordered_map<ObjectId,E>::iterator i){pb_-=i->second.size;porder_.erase(i->second.pos);p_.erase(i);}
    void erase_any(ObjectId x){auto ci=c_.find(x);if(ci!=c_.end())erase_c(ci);auto pi=p_.find(x);if(pi!=p_.end())erase_p(pi);}
    void insert_c(ObjectId x,Bytes s){erase_any(x);corder_.push_back(x);c_[x]={s,std::prev(corder_.end())};cb_+=s;}
    void insert_p(ObjectId x,Bytes s){erase_any(x);porder_.push_back(x);p_[x]={s,std::prev(porder_.end())};pb_+=s;}
    void evict_c(){if(corder_.empty())return;erase_c(c_.find(corder_.front()));}
    void evict_p(){if(porder_.empty())return;erase_p(p_.find(porder_.front()));}
    void reclaim(){
        while(used()>capacity_){
            const Bytes pq=capacity_-cq_; const bool c_over=cb_>cq_,p_over=pb_>pq;
            if(c_over&&(!p_over||cb_-cq_>=pb_-pq))evict_c();
            else if(p_over)evict_p(); else if(!porder_.empty())evict_p(); else evict_c();
        }
        check();
    }
    Bytes capacity_=0,cq_=0,cb_=0,pb_=0;
    std::list<ObjectId> corder_,porder_; std::unordered_map<ObjectId,E> c_,p_;
};
}
