#pragma once
#include <algorithm>
#include <cstdint>
#include <list>
#include <limits>
#include <unordered_map>
#include <vector>
namespace lapsim {
class A2ByteRankStack {
public:
 using ObjectId=std::uint64_t; using Bytes=std::uint64_t;
 explicit A2ByteRankStack(Bytes max):max_(max){}
 Bytes rank(ObjectId x,Bytes req)const{auto it=m_.find(x);if(it==m_.end()||it->second.size<req)return inf();Bytes r=it->second.size;for(auto p=std::next(it->second.pos);p!=order_.end();++p)r+=m_.at(*p).size;return r;}
 void touch(ObjectId x,Bytes s){if(!s)return;auto it=m_.find(x);if(it!=m_.end()){s=std::max(s,it->second.size);bytes_-=it->second.size;order_.erase(it->second.pos);m_.erase(it);}if(s>max_)return;order_.push_back(x);m_[x]={s,std::prev(order_.end())};bytes_+=s;trim();}
 Bytes consume(ObjectId x,Bytes req){Bytes r=rank(x,req);auto it=m_.find(x);if(it!=m_.end()){bytes_-=it->second.size;order_.erase(it->second.pos);m_.erase(it);}return r;}
 static constexpr Bytes inf(){return std::numeric_limits<Bytes>::max();}
private:
 struct E{Bytes size;std::list<ObjectId>::iterator pos;};
 void trim(){while(bytes_>max_&&!order_.empty()){auto x=order_.front();auto it=m_.find(x);bytes_-=it->second.size;order_.pop_front();m_.erase(it);}}
 Bytes max_=0,bytes_=0;std::list<ObjectId>order_;std::unordered_map<ObjectId,E>m_;
};
class A2Map {
public:
 using Bytes=std::uint64_t;using ObjectId=std::uint64_t;
 struct Score{Bytes C=0,P=0,demands=0,hits=0,reactive=0,residual=0;};
 A2Map(Bytes total,int parts=10,Bytes interval=100ULL*1024ULL*1024ULL):total_(total),parts_(std::max(2,parts)),interval_(std::max<Bytes>(1,interval)),cstack_(total),pstack_(total){reset_candidates();}
 void before_demand(ObjectId x,Bytes s){auto cr=cstack_.rank(x,s);auto pr=pstack_.consume(x,s);cstack_.touch(x,s);for(auto &q:scores_){++q.demands;bool rh=cr<=q.C,ph=pr<=q.P;if(rh)++q.reactive;if(!rh&&ph)++q.residual;if(rh||ph)++q.hits;}epoch_bytes_+=s;}
 void observe_prediction(ObjectId x,Bytes s){pstack_.touch(x,s);++predictions_;}
 bool due()const{return epoch_bytes_>=interval_;}
 Bytes choose(Bytes oldC){size_t b=0;for(size_t i=1;i<scores_.size();++i){auto &x=scores_[i],&y=scores_[b];Bytes xd=x.C>oldC?x.C-oldC:oldC-x.C,yd=y.C>oldC?y.C-oldC:oldC-y.C;if(x.hits>y.hits||(x.hits==y.hits&&(xd<yd||(xd==yd&&x.P<y.P))))b=i;}last_=scores_[b];++epochs_;if(last_.C!=oldC)++resizes_;epoch_bytes_=0;predictions_=0;for(auto&q:scores_)q.demands=q.hits=q.reactive=q.residual=0;return last_.C;}
 const Score& last()const{return last_;}Bytes epochs()const{return epochs_;}Bytes resizes()const{return resizes_;}
private:
 void reset_candidates(){scores_.clear();for(int j=0;j<parts_;++j){Bytes P=(total_*(Bytes)j)/(Bytes)parts_;scores_.push_back({total_-P,P,0,0,0,0});}}
 Bytes total_,interval_,epoch_bytes_=0,predictions_=0,epochs_=0,resizes_=0;int parts_;A2ByteRankStack cstack_,pstack_;std::vector<Score>scores_;Score last_;
};
}
