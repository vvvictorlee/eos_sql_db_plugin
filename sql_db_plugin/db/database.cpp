// #include "database.hpp"
#include <eosio/sql_db_plugin/database.hpp>

namespace eosio
{

    database::database(const std::string &uri, uint32_t block_num_start, size_t pool_size) {
        // m_session = std::make_shared<soci::session>(uri); 
        m_session_pool = std::make_shared<soci_session_pool>(pool_size,uri);
        m_accounts_table = std::make_unique<accounts_table>(m_session_pool);
        m_blocks_table = std::make_unique<blocks_table>(m_session_pool);
        m_traces_table = std::make_unique<traces_table>(m_session_pool);
        m_transactions_table = std::make_unique<transactions_table>(m_session_pool);
        m_actions_table = std::make_unique<actions_table>(m_session_pool);
        m_block_num_start = block_num_start;
        system_account = chain::name(chain::config::system_account_name).to_string();
    }

    database::database(const std::string &uri, uint32_t block_num_start, size_t pool_size, std::vector<string> filter_on, std::vector<string> filter_out) {
        new (this)database(uri,block_num_start,pool_size);
        m_action_filter_on = filter_on;
        m_contract_filter_out = filter_out;
    }

    void database::wipe() {
        chain::abi_def abi_def;
        abi_def = eosio_contract_abi(abi_def);
        m_accounts_table->add_eosio(system_account, fc::json::to_string( abi_def ));
    }

    bool database::is_started() {
        ilog(system_account);
        return m_accounts_table->exist(system_account);
    }

    void database::consume_block_state( const chain::block_state_ptr& bs) {
        //TODO
        m_blocks_table->add(bs);
    }

    void database::consume_irreversible_block_state( const chain::block_state_ptr& bs ,boost::mutex::scoped_lock& lock_db, boost::condition_variable& condition, boost::atomic<bool>& exit){
        //TODO
        // ilog("run consume irreversible block");
        auto block_id = bs->id.str();

        // sleep(1);
        // do{
        //     bool update_irreversible = m_blocks_table->irreversible_set(block_id, true);
        //     if(update_irreversible || exit) break;
        //     else condition.timed_wait(lock_db, boost::posix_time::milliseconds(10));
        // }while(!exit);

        for(auto& receipt : bs->block->transactions) {
            string trx_id_str;
            if( receipt.trx.contains<chain::packed_transaction>() ){
                const auto& tm = chain::transaction_metadata(receipt.trx.get<chain::packed_transaction>());

                if(tm.trx.actions.size()==1 && tm.trx.actions[0].name.to_string() == "onblock" ) continue ;

                // for(auto actions : trx.actions){
                //     m_actions_table->add(actions,trx.id(), bs->block->timestamp, m_action_filter_on);
                // }

                if(tm.trx.actions.size()==1 && std::find(m_contract_filter_out.begin(),m_contract_filter_out.end(),tm.trx.actions[0].account.to_string()) != m_contract_filter_out.end() ){
                    continue;
                }

                trx_id_str = tm.trx.id().str();
                

            }else{
                trx_id_str = receipt.trx.get<chain::transaction_id_type>().str();
            }

            string trace_result;
            do{
                trace_result = m_traces_table->list(trx_id_str, bs->block->timestamp);
                if( !trace_result.empty() ){
                    auto traces = fc::json::from_string(trace_result).as<chain::transaction_trace>();
                    for(auto atc : traces.action_traces){
                        if( atc.receipt.receiver == atc.act.account ){
                            m_actions_table->add(atc.act, trx_id_str, bs->block->timestamp, m_action_filter_on);
                        }
                    }
                    m_traces_table->parse_traces(traces);
                    break;
                } else if(exit) {
                    break;
                }else condition.timed_wait(lock_db, boost::posix_time::milliseconds(10));     
                ilog( "${tx_id}",("tx_id",trx_id_str) );

            }while((!exit));
            

        }

    }

    void database::consume_transaction_metadata( const chain::transaction_metadata_ptr& tm ) {

        // if(tm->trx.actions.size()==1 && tm->trx.actions[0].name.to_string() == "onblock" ) return ;

        m_transactions_table->add(tm->trx);
        for(auto actions : tm->trx.actions){
            m_actions_table->add(actions,tm->trx.id(), tm->trx.expiration, m_action_filter_on);
        }

    }

    void database::consume_transaction_trace( const chain::transaction_trace_ptr& tt) {
        m_traces_table->add(tt);
    }

    const std::string database::block_states_col = "block_states";
    const std::string database::blocks_col = "blocks";
    const std::string database::trans_col = "transactions";
    const std::string database::trans_traces_col = "transaction_traces";
    const std::string database::actions_col = "actions";
    const std::string database::accounts_col = "accounts";

} // namespace
