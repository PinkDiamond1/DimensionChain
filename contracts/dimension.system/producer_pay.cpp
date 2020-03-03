#include "eosio.system.hpp"

#include <eosio.token/eosio.token.hpp>

namespace eosiosystem {

   const int64_t  min_pervote_daily_pay = 100'0000;
   const int64_t  min_activated_stake   = 150'000'000'0000;
   const int64_t  min_proposal_stake   = 1'000'000;
   const double   continuous_rate       = 0.04879;          // 5% annual rate
   const double   perblock_rate         = 0.0025;           // 0.25%
   const double   standby_rate          = 0.0075;           // 0.75%
   const uint32_t blocks_per_year       = 52*7*24*2*3600;   // half seconds per year
   const uint32_t seconds_per_year      = 52*7*24*3600;
   const uint32_t blocks_per_day        = 2 * 24 * 3600;
   const uint32_t blocks_per_hour       = 2 * 3600;
   const uint64_t useconds_per_day      = 24 * 3600 * uint64_t(1000000);
   const uint64_t useconds_per_year     = seconds_per_year*1000000ll;

   const uint16_t min_producer_size     = 7;  //避免bp太少就接替eonio出块


   void system_contract::onblock( block_timestamp timestamp, account_name producer ) {
      using namespace eosio;

      require_auth(N(eonio));

      /** until activated stake crosses this threshold no new rewards are paid */
      if( _gstate.total_proposal_stake < min_proposal_stake || _gstate.producer_num < min_producer_size )
         return;

      if( _gstate.last_pervote_bucket_fill == 0 )  /// start the presses
         _gstate.last_pervote_bucket_fill = current_time();


      /**
       * At startup the initial producer may not be one that is registered / elected
       * and therefore there may be no producer object for them.
       */
      auto prod = _producers.find(producer);
      if ( prod != _producers.end() ) {
         _gstate.total_unpaid_blocks++;
         _producers.modify( prod, 0, [&](auto& p ) {
               p.unpaid_blocks++;
         });
      }

      /// only update block producers once every minute, block_timestamp is in half seconds
      if( timestamp.slot - _gstate.last_producer_schedule_update.slot > 240 ) {
         update_elected_producers( timestamp );

         if( (timestamp.slot - _gstate.last_name_close.slot) > blocks_per_day ) {
            name_bid_table bids(_self,_self);
            auto idx = bids.get_index<N(highbid)>();
            auto highest = idx.begin();
            if( highest != idx.end() &&
                highest->high_bid > 0 &&
                highest->last_bid_time < (current_time() - useconds_per_day) &&
                _gstate.thresh_activated_stake_time > 0 &&
                (current_time() - _gstate.thresh_activated_stake_time) > 14 * useconds_per_day ) {
                   _gstate.last_name_close = timestamp;
                   idx.modify( highest, 0, [&]( auto& b ){
                         b.high_bid = -b.high_bid;
               });
            }
         }
      }
   }

   using namespace eosio;

   uint16_t system_contract::get_producers_size() {
       uint16_t count = 0;
       auto idx = _producers.get_index<N(prototalvote)>();

       for ( auto it = idx.cbegin(); it != idx.cend(); ++it ) {
          ++ count;
       }
       return count;
   }

   void system_contract::execproposal( const account_name owner, uint64_t proposal_id ) {
       require_auth( owner );
       const auto now_time = now();

       auto gnd = _gnode.find( owner );
       eosio_assert(gnd != _gnode.end(), "only governance node can exec proposal");

       auto prop = _proposals.find( proposal_id );
       eosio_assert(prop != _proposals.end(), "proposal_id not in _proposals");
       if(get_producers_size() > 7) { // 多于7个时检查
           eosio_assert(now_time > prop->vote_end_time, "proposal not end");
       }
       eosio_assert(now_time < prop->exec_end_time, "proposal execution time has elapsed");
       eosio_assert( ! prop->is_exec, "proposal has been executed");
            
      // 检查proposal == 1是否满足条件，是这执行
        if( prop->type == 1 ) {
            if(prop->total_yeas - prop->total_nays > _gstate.total_proposal_stake / 10) {  // 提案是否满足条件 yeas-nays > staked/10 ?
                _proposals.modify(prop, owner, [&](auto &info) {
                    info.is_satisfy = true;
                });
                gnd = _gnode.find( prop->account );
                eosio_assert(gnd != _gnode.end(), "account not in _gnode");

                add_elected_producers( prop->account);
                _gstate.producer_num ++;
                _gnode.modify( gnd, owner, [&](auto& info) {
                    info.is_bp   = true;
                });
            }
        }
      // 检查proposal == 2是否满足条件，是这执行
        if( prop->type == 2 ) {
            if(prop->total_yeas - prop->total_nays > _gstate.total_proposal_stake / 10) {  // 提案是否满足条件 yeas-nays > staked/10 ?
                _proposals.modify(prop, owner, [&](auto &info) {
                    info.is_satisfy = true;
                });
                gnd = _gnode.find( prop->account );
                eosio_assert(gnd != _gnode.end(), "account not in _gnode");

                remove_elected_producers( prop->account);
                _gstate.producer_num --;
                _gnode.modify( gnd, owner, [&](auto& info) {
                    info.is_bp   = false;
                });
            }
        }
      // 检查proposal == 3是否满足条件，是这执行
        if( prop->type == 3 ) {
            if(prop->total_yeas - prop->total_nays > _gstate.total_proposal_stake / 10) {  // 提案是否满足条件 yeas-nays > staked/10 ?
                _proposals.modify(prop, owner, [&](auto &info) {
                    info.is_satisfy = true;
                });
                set_consensus_type(prop->consensus_type);
            }
        }

        _proposals.modify(prop, owner, [&](auto &info) {
            info.is_exec = true;
            info.exec_time = now_time;
            info.total_staked = _gstate.total_proposal_stake;
        });

   }

   //发起提案，只有gnode才可以发起提案。
   // type 1: add bp 2: remove bp 3: switch consensus 
   void system_contract::newproposal( const account_name owner, const account_name account, uint32_t block_height, int64_t type, int64_t consensus_type) {
       require_auth( owner );
       const auto now_time = now();

       auto gnd = _gnode.find( owner );
       eosio_assert(gnd != _gnode.end(), "only governance node can new proposal");

       //不能提名其他账号
       if(type == 1) {
           eosio_assert(owner == account, "can not add other account to bp");
       }
       //account必须是出块节点
       if(type == 2){

       }
       //切换共识，对共识类型检查
       if (type == 3) {
           eosio_assert(consensus_type == 0
                     || consensus_type == 1
                     || consensus_type == 2, "consensus_type must be one of 0, 1, 2");
       }

       int64_t fee = _gstate.new_proposal_fee;
       INLINE_ACTION_SENDER(eosio::token, transfer)(
          N(eonio.token), { {owner, N(active)} },
          { owner, N(eonio.prop), asset(fee), "transfer 1.5000 EON to new proposal" }
       );

       uint64_t id = _proposals.available_primary_key();

       _proposals.emplace(_self, [&](auto &info) {
           info.id = id;
           info.owner = owner;
           info.account = account;
           info.start_time = now_time;
           if(type == 1 || type == 2) {
               info.vote_end_time = now_time + 24*3600 * 1;
           } else {
               info.vote_end_time = now_time + 24*3600 * 1;
           }
           info.vote_end_time = now_time + 2*3600; //测试
           info.exec_end_time = info.vote_end_time + 24*3600 * 3;
           info.block_height = block_height;
           info.type = type;
           info.is_satisfy = false;
           info.is_exec = false;
           info.consensus_type = consensus_type;
           info.total_yeas     = 0;
           info.total_nays     = 0;
           info.total_staked   = 0;
       });

       _gstate.proposal_num += 1;

   }

   // 抵押EON成为governance node, 可以发起提案
   void system_contract::staketognode( const account_name payer, const account_name owner, const public_key& producer_key, const std::string& url, uint16_t location ) {
       require_auth( payer );
       const auto now_time = now();

       auto gnd = _gnode.find( owner );
       eosio_assert(gnd == _gnode.end(), "account already in _gnode");

       int64_t fee = _gstate.stake_to_gnode_fee;
       INLINE_ACTION_SENDER(eosio::token, transfer)(
          N(eonio.token), { {payer, N(active)} },
          { payer, N(eonio.bpstk), asset(fee), "stake 1.0000 EON to governance node" }
       );

       gnd = _gnode.emplace( owner, [&]( goverance_node_info& info  ) {
            info.owner          = owner;
            info.payer          = payer;
            info.bp_staked      = fee;
            info.stake_time     = now_time;
            info.is_bp          = false;
            info.status         = 0;
            info.producer_key   = producer_key;
            info.url            = url;
            info.location       = location;
       });
   }

   // unstakegnode
   void system_contract::unstakegnode( const account_name owner ) {
       require_auth( owner );
       const auto now_time = now();

       auto gnd = _gnode.find( owner );
       eosio_assert(gnd != _gnode.end(), "account not in _gnode");
       eosio_assert(!gnd->is_bp, "account is bp, can not unstake");

       auto idx = _proposals.get_index<N(byvendtime)>();
       for(auto it = idx.cbegin(); it != idx.cend(); ++it) {
             if(it->vote_end_time  <= now_time) continue;
 
             eosio_assert(it->owner != owner, "proposal owner is equal owner");
             eosio_assert(it->account != owner, "proposal account is equal owner");
       }

       int64_t fee = gnd->bp_staked;
       account_name receiver = gnd->payer;
       
       _gnode.erase( gnd );

       INLINE_ACTION_SENDER(eosio::token, transfer)(
          N(eonio.token), { {N(eonio.bpstk), N(active)} },
          { N(eonio.bpstk), receiver, asset(fee), "unstake goverance node refund" }
       );
   }

   // 更新governance node信息
   void system_contract::updategnode( const account_name owner, const public_key& producer_key, const std::string& url, uint16_t location ) {
       require_auth( owner );

       auto gnd = _gnode.find( owner );
       eosio_assert(gnd != _gnode.end(), "account not in _gnode");

       eosio_assert(!gnd->is_bp, "can not unstake, this account is bp now");

       // 检查是否有与该账号相关的提案

      _gnode.modify( gnd, owner, [&](auto& info) {
         info.producer_key   = producer_key;
         info.url            = url;
         info.location       = location;
      });

   }


   void system_contract::claimrewards( const account_name& owner ) {
      require_auth(owner);

      const auto& prod = _producers.get( owner );
      eosio_assert( prod.active(), "producer does not have an active key" );

      eosio_assert( _gstate.total_proposal_stake >= min_proposal_stake,
                    "cannot claimrewards until the chain is activated (at least 1 000 000)" );

      auto ct = current_time();

      eosio_assert( ct - prod.last_claim_time > useconds_per_day, "already claimed rewards within past day" );


      int64_t producer_per_block_pay = 0;
      if( _gstate.total_unpaid_blocks > 0 ) {
         producer_per_block_pay = _gstate.reward_pre_block * prod.unpaid_blocks;
      }
      
      _gstate.perblock_bucket     -= producer_per_block_pay;
      _gstate.total_unpaid_blocks -= prod.unpaid_blocks;

      _producers.modify( prod, 0, [&](auto& p) {
          p.last_claim_time = ct;
          p.unpaid_blocks = 0;
      });

      if( producer_per_block_pay > 0 ) {
         INLINE_ACTION_SENDER(eosio::token, transfer)( N(eonio.token), {N(eonio.blkpay),N(active)},
                                                       { N(eonio.blkpay), owner, asset(producer_per_block_pay), std::string("producer block pay") } );
      }
      
   }

} //namespace eosiosystem
