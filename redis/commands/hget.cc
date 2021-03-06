#include "redis/commands/hget.hh"
#include "redis/commands/unexpected.hh"
#include "redis/request.hh"
#include "redis/reply.hh"
#include "redis/redis_mutation.hh"
#include "redis/prefetcher.hh"
#include "timeout_config.hh"
#include "service/client_state.hh"
#include "service/storage_proxy.hh"
#include "db/system_keyspace.hh"
#include "partition_slice_builder.hh"
#include "gc_clock.hh"
#include "dht/i_partitioner.hh"
#include "log.hh"
#include <boost/range/adaptor/transformed.hpp>
#include <boost/range/algorithm_ext/push_back.hpp>
#include <boost/range/adaptor/filtered.hpp>
#include <boost/range/adaptor/indirected.hpp>
namespace redis {
namespace commands {

shared_ptr<abstract_command> hget::prepare(service::storage_proxy& proxy, const service::client_state& cs, request&& req, bool multi)
{
    if (req._args_count < 2 || (!multi && req._args_count != 2)) {
        return unexpected::prepare(std::move(req._command), std::move(bytes {"-ERR wrong number of arguments for HMGET\r\n"}));
    }
    std::vector<bytes> map_keys;
    for (size_t i = 1; i < req._args_count; i++) {
        map_keys.emplace_back(req._args[i]);
    }
    return seastar::make_shared<hget>(std::move(req._command), maps_schema(proxy, cs.get_keyspace()), std::move(req._args[0]), std::move(map_keys), multi);
}
template<typename Type>
shared_ptr<abstract_command> prepare_impl(service::storage_proxy& proxy, const service::client_state& cs, request&& req) {
    if (req._args_count != 1) {
        return unexpected::prepare(std::move(req._command), std::move(bytes { msg_syntax_err }) );
    }
    return seastar::make_shared<Type>(std::move(req._command), maps_schema(proxy, cs.get_keyspace()), std::move(req._args[0]));
}
shared_ptr<abstract_command> hkeys::prepare(service::storage_proxy& proxy, const service::client_state& cs, request&& req) {
    return prepare_impl<hkeys>(proxy, cs, std::move(req));
}
shared_ptr<abstract_command> hvals::prepare(service::storage_proxy& proxy, const service::client_state& cs, request&& req) {
    return prepare_impl<hvals>(proxy, cs, std::move(req));
}
shared_ptr<abstract_command> hgetall::prepare(service::storage_proxy& proxy, const service::client_state& cs, request&& req) {
    return prepare_impl<hgetall>(proxy, cs, std::move(req));
}
future<redis_message> hget::execute(service::storage_proxy& proxy, db::consistency_level cl, db::timeout_clock::time_point now, const timeout_config& tc, service::client_state& cs)
{
    auto timeout = now + tc.read_timeout;
    return prefetch_map(proxy, _schema, _key, _map_keys, fetch_options::values, cl, timeout, cs).then([this, &proxy, cl, timeout, &cs] (auto pd) {
        if (pd && pd->has_data()) {
            auto&& vals = boost::copy_range<std::vector<std::optional<bytes>>> (pd->data() | boost::adaptors::transformed([this] (auto& data) {
                return std::move(data.first); 
            }));
            return redis_message::make(std::move(vals));
        }
        return redis_message::null();
    });
}
future<redis_message> hall::execute(service::storage_proxy& proxy, db::consistency_level cl, db::timeout_clock::time_point now, const timeout_config& tc, service::client_state& cs)
{
    auto timeout = now + tc.read_timeout;
    return prefetch_map(proxy, _schema, _key, _option, cl, timeout, cs).then([this, &proxy, cl, timeout, &cs] (auto pd) {
        if (pd && pd->has_data()) {
            std::vector<std::optional<bytes>> result;
            if (_option == redis::fetch_options::keys) {
                auto&& keys = boost::copy_range<std::vector<std::optional<bytes>>> (pd->data() | boost::adaptors::transformed([this] (auto& data) {
                    return std::move(data.first); 
                }));
                result = std::move(keys);
            } else if (_option == redis::fetch_options::values) {
                // values was saved in the first of the pair.
                auto&& vals = boost::copy_range<std::vector<std::optional<bytes>>> (pd->data() | boost::adaptors::transformed([this] (auto& data) {
                    return std::move(data.first); 
                }));
                result = std::move(vals);
            } else {
                result.reserve(pd->data().size() * 2);
                for (auto&& data : pd->data()) {
                    result.emplace_back(data.first);
                    result.emplace_back(data.second);
                }
            }
            return redis_message::make(std::move(result));
        }
        return redis_message::null();
    });
}

}
}
