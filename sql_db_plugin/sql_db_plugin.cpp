/**
 *  @file
 *  @copyright defined in eos/LICENSE.txt
 *  @author Alessandro Siniscalchi <asiniscalchi@gmail.com>
 */
#include <eosio/sql_db_plugin/sql_db_plugin.hpp>
// #include "database.hpp"
#include "consumer.hpp"

#include <fc/io/json.hpp>
#include <fc/utf8.hpp>
#include <fc/variant.hpp>

#include <boost/algorithm/string.hpp>

namespace {
const char* BLOCK_START_OPTION = "sql_db-block-start";
const char* BUFFER_SIZE_OPTION = "sql_db-queue-size";
const char* SQL_DB_URI_OPTION = "sql_db-uri";
const char* SQL_DB_ACTION_FILTER_ON = "sql_db-action-filter-on";
const char* SQL_DB_CONTRACT_FILTER_OUT = "sql_db-contract-filter-out";
}

namespace fc { class variant; }

namespace eosio {

    static appbase::abstract_plugin& _sql_db_plugin = app().register_plugin<sql_db_plugin>();

    class sql_db_plugin_impl  {
        public:
            sql_db_plugin_impl(){};
            ~sql_db_plugin_impl(){};

            chain_plugin* chain_plug = nullptr;
            std::shared_ptr<sql_database> sql_db;

            std::unique_ptr<consumer> handler;
            std::vector<std::string> contract_filter_out;

            fc::optional<boost::signals2::scoped_connection> accepted_block_connection;
            fc::optional<boost::signals2::scoped_connection> irreversible_block_connection;
            fc::optional<boost::signals2::scoped_connection> accepted_transaction_connection;
            fc::optional<boost::signals2::scoped_connection> applied_transaction_connection;

            void accepted_block( const chain::block_state_ptr& );
            void applied_irreversible_block( const chain::block_state_ptr& );
            void accepted_transaction( const chain::transaction_metadata_ptr& );
            void applied_transaction( const chain::transaction_trace_ptr& );

            bool filter_out_contract( std::string contract) {
                if( std::find(contract_filter_out.begin(),contract_filter_out.end(),contract) != contract_filter_out.end() ){
                    return true;
                }
                return false;
            }

    };

    void sql_db_plugin_impl::accepted_block( const chain::block_state_ptr& bs ) {
        handler->push_block_state(bs);
    }


    sql_db_plugin::sql_db_plugin():my(new sql_db_plugin_impl ){}

    sql_db_plugin::~sql_db_plugin(){}

    sql_db_apis::read_only  sql_db_plugin::get_read_only_api()const { 

        return sql_db_apis::read_only(my->chain_plug->chain(),my->chain_plug->get_abi_serializer_max_time(),my->sql_db); 
    }

    void sql_db_plugin::set_program_options(options_description& cli, options_description& cfg) {
        dlog("set_program_options");

        cfg.add_options()
                (BUFFER_SIZE_OPTION, bpo::value<uint>()->default_value(5000),
                "The queue size between nodeos and SQL DB plugin thread.")
                (BLOCK_START_OPTION, bpo::value<uint32_t>()->default_value(0),
                "The block to start sync.")
                (SQL_DB_URI_OPTION, bpo::value<std::string>(),
                "Sql DB URI connection string"
                " If not specified then plugin is disabled. Default database 'EOS' is used if not specified in URI.")
                (SQL_DB_ACTION_FILTER_ON,bpo::value<std::string>(),
                "saved action with filter on")
                (SQL_DB_CONTRACT_FILTER_OUT,bpo::value<std::string>(),
                "saved action without filter out")
                ;
    }

    void sql_db_plugin::plugin_initialize(const variables_map& options) {
        ilog("initialize");
        std::vector<std::string> action_filter_on;
        if( options.count( SQL_DB_ACTION_FILTER_ON ) ){
            auto fo = options.at(SQL_DB_ACTION_FILTER_ON).as<std::string>();
            boost::replace_all(fo," ","");
            boost::split(action_filter_on, fo,  boost::is_any_of( "," ));
        }

        if( options.count( SQL_DB_CONTRACT_FILTER_OUT ) ){
            auto fo = options.at(SQL_DB_CONTRACT_FILTER_OUT).as<std::string>();
            boost::replace_all(fo," ","");
            boost::split(my->contract_filter_out, fo,  boost::is_any_of( "," ));
        }

        std::string uri_str = options.at(SQL_DB_URI_OPTION).as<std::string>();
        if (uri_str.empty()){
            wlog("db URI not specified => eosio::sql_db_plugin disabled.");
            return;
        }

        ilog("connecting to ${u}", ("u", uri_str));
        uint32_t block_num_start = options.at(BLOCK_START_OPTION).as<uint32_t>();
        auto queue_size = options.at(BUFFER_SIZE_OPTION).as<uint32_t>();

        ilog("queue size ${size}",("size",queue_size));

        //for three thread。 TODO: change to thread db pool
        my->sql_db = std::make_shared<sql_database>(uri_str, block_num_start, 1);
        auto db_blocks = std::make_unique<sql_database>(uri_str, block_num_start, 5, action_filter_on,my->contract_filter_out);

        if (!db_blocks->is_started()) {
            if (block_num_start == 0) {
                ilog("Resync requested: wiping database");
                db_blocks->wipe();
            }
        }

        my->handler = std::make_unique<consumer>(std::move(db_blocks),queue_size);
        my->chain_plug = app().find_plugin<chain_plugin>();

        FC_ASSERT(my->chain_plug);
        auto& chain = my->chain_plug->chain();
        
        my->accepted_block_connection.emplace(chain.accepted_block.connect([this]( const chain::block_state_ptr& bs){
            my->accepted_block(bs);
        } ));   
    }

    void sql_db_plugin::plugin_startup() {
        ilog("startup");
    }

    void sql_db_plugin::plugin_shutdown() {
        ilog("shutdown");
        // my->handler->shutdown();
        // my->accepted_block_connection.reset();
        // my->irreversible_block_connection.reset();
        // my->accepted_transaction_connection.reset();
        // my->applied_transaction_connection.reset();
    }

    namespace sql_db_apis{
        
        read_only::get_tokens_result read_only::get_tokens( const get_tokens_params& p )const {
            get_tokens_result result;

            for(auto t : p.tokens){
                token tk;
                tk.code = t.code;
                tk.symbol = t.symbol;

                walk_key_value_table(t.code, p.account, N(accounts), [&](const key_value_object& obj){
                    EOS_ASSERT( obj.value.size() >= sizeof(asset), chain::asset_type_exception, "Invalid data on table");

                    asset cursor;
                    fc::datastream<const char *> ds(obj.value.data(), obj.value.size());
                    fc::raw::unpack(ds, cursor);

                    EOS_ASSERT( cursor.get_symbol().valid(), chain::asset_type_exception, "Invalid asset");

                    if( cursor.symbol_name() == t.symbol ) {
                        tk.quantity = cursor;
                        tk.symbol_precision = cursor.decimals();
                        result.tokens.emplace_back(tk);
                    }

                    // return false if we are looking for one and found it, true otherwise
                    return !(cursor.symbol_name() == t.symbol);
                },[&](){
                    if( t.symbol_precision > 18 ) return ;
                    tk.quantity = asset(0, chain::symbol(chain::string_to_symbol(t.symbol_precision,t.symbol.c_str())));
                    tk.symbol_precision = t.symbol_precision;
                    result.tokens.emplace_back(tk);
                });
            }
            return result;
        }

        read_only::get_all_tokens_result read_only::get_all_tokens( const get_all_tokens_params& p )const {
            get_all_tokens_result result;

            if(p.startNum<0 || p.pageSize<0) return result;

            try{
                auto assets = sql_db->m_actions_table->get_assets(sql_db->m_session_pool->get_session(), p.startNum, p.pageSize);

                for(auto it = assets.begin() ; it != assets.end(); it++){
                    token t;
                    t.code = it->get<string>(0);
                    t.symbol = it->get<string>(3);

                    walk_key_value_table(t.code, p.account, N(accounts), [&](const key_value_object& obj){
                        EOS_ASSERT( obj.value.size() >= sizeof(asset), chain::asset_type_exception, "Invalid data on table");

                        asset cursor;
                        fc::datastream<const char *> ds(obj.value.data(), obj.value.size());
                        fc::raw::unpack(ds, cursor);

                        EOS_ASSERT( cursor.get_symbol().valid(), chain::asset_type_exception, "Invalid asset");

                        if( cursor.symbol_name() == t.symbol ) {
                            t.quantity = cursor;
                            t.symbol_precision = cursor.decimals();
                            result.tokens.emplace_back(t);
                        }

                        // return false if we are looking for one and found it, true otherwise
                        return !(cursor.symbol_name() == t.symbol);

                    }, [&](){
                        t.quantity = asset(0, chain::symbol(chain::string_to_symbol(it->get<int>(2),t.symbol.c_str())));
                        t.symbol_precision = it->get<int>(2);
                        result.tokens.emplace_back(t);
                    });

                }
            } catch(std::exception e) {
                wlog("${e}",("e",e.what()));
            } catch (...) {
                wlog("unknown");
            }
            
            return result;
        }

        read_only::get_userresource_result read_only::get_userresource( const get_userresource_params& p )const {
            get_userresource_result result;

            abi_def abi;
            const auto& code_account = db.db().get<account_object,by_name>( N(eosio) );
            if(abi_serializer::to_abi(code_account.abi, abi)){
                abi_serializer abis( abi, abi_serializer_max_time );
                walk_key_value_table(N(eosio), p.account, N(userres), [&](const key_value_object& obj){
                    EOS_ASSERT( obj.value.size() >= sizeof(get_userresource_result), chain::asset_type_exception, "Invalid data on table");

                    fc::datastream<const char *> ds(obj.value.data(), obj.value.size());
                    auto userr = abis.binary_to_variant( "user_resources", ds, abi_serializer_max_time );
                    result.net_weight = userr["net_weight"].as<asset>();
                    result.cpu_weight = userr["cpu_weight"].as<asset>();
                    result.ram_bytes = userr["ram_bytes"].as<int64_t>();


                    return true;
                },[&](){});
            }
            return result;
        }

        read_only::get_refund_result read_only::get_refund( const get_refund_params& p )const {
            get_refund_result result;

            abi_def abi;
            const auto& code_account = db.db().get<account_object,by_name>( N(eosio) );
            if(abi_serializer::to_abi(code_account.abi, abi)){
                abi_serializer abis( abi, abi_serializer_max_time );
                
                walk_key_value_table(N(eosio), p.account, N(refunds), [&](const key_value_object& obj){
                    EOS_ASSERT( obj.value.size() >= sizeof(get_userresource_result), chain::asset_type_exception, "Invalid data on table");
                    
                    fc::datastream<const char *> ds(obj.value.data(), obj.value.size());
                    auto ref = abis.binary_to_variant( "refund_request", ds, abi_serializer_max_time );
                    result.request_time = ref["request_time"].as<string>();
                    result.net_amount = ref["net_amount"].as<asset>();
                    result.cpu_amount = ref["cpu_amount"].as<asset>();

                    return true;
                },[&](){});
            }
            return result;
        }

        template<typename Function, typename Function2>
        void read_only::walk_key_value_table(const name& code, const name& scope, const name& table, Function f, Function2 f2) const
        {
            const auto& d = db.db();
            const auto* t_id = d.find<chain::table_id_object, chain::by_code_scope_table>(boost::make_tuple(code, scope, table));
            if (t_id != nullptr) {
                const auto &idx = d.get_index<chain::key_value_index, chain::by_scope_primary>();
                decltype(t_id->id) next_tid(t_id->id._id + 1);
                auto lower = idx.lower_bound(boost::make_tuple(t_id->id));
                auto upper = idx.lower_bound(boost::make_tuple(next_tid));
                for (auto itr = lower; itr != upper; ++itr) {
                    if (!f(*itr)) {
                        break;
                    }
                }
            } else {
                f2();
            }
        }

    }//namespace sql_db_apis

} // namespace eosio
