#include "redis/commands/get.hh"
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

shared_ptr<abstract_command> get::prepare(service::storage_proxy& proxy, const service::client_state& cs, request&& req)
{
    if (req._args_count != 1) {
        return unexpected::prepare(std::move(req._command), std::move(to_bytes(sprint("-wrong number of arguments (given %ld, expected 1)\r\n", req._args_count))));
    }
    return make_shared<get>(std::move(req._command), simple_objects_schema(proxy, cs.get_keyspace()), std::move(req._args[0]));
}

future<redis_message> get::execute(service::storage_proxy& proxy, db::consistency_level cl, db::timeout_clock::time_point now, const timeout_config& tc, service::client_state& cs)
{
    auto timeout = now + tc.read_timeout;
    auto fetched = prefetch_simple(proxy, _schema, _key, cl, timeout, cs);
    return fetched.then([this, &proxy, cl, timeout, &cs] (auto pd) {
        if (pd && pd->has_data()) {
            return redis_message::make(std::move(pd->_data));
        }
        return redis_message::null();
    });
}

shared_ptr<abstract_command> getset::prepare(service::storage_proxy& proxy, const service::client_state& cs, request&& req)
{
    if (req._args_count < 2) {
        return unexpected::prepare(std::move(req._command), std::move(to_bytes(sprint("-wrong number of arguments (given %ld, expected 1)\r\n", req._args_count))));
    }
    return make_shared<getset>(std::move(req._command), simple_objects_schema(proxy, cs.get_keyspace()), std::move(req._args[0]), std::move(req._args[1]));
}

future<redis_message> getset::execute(service::storage_proxy& proxy, db::consistency_level cl, db::timeout_clock::time_point now, const timeout_config& tc, service::client_state& cs)
{
    auto timeout = now + tc.read_timeout;
    return prefetch_simple(proxy, _schema, _key, cl, timeout, cs).then([this, &proxy, cl, timeout, &cs] (auto pd) {
        return redis::write_mutation(proxy, redis::make_simple(_schema, _key, std::move(_data)), cl, timeout, cs).then_wrapped([this, pd = std::move(pd)] (auto f) {
            try {
                f.get();
            } catch(...) {
                return redis_message::err();
            }
            if (pd && pd->has_data()) {
                return redis_message::make(std::move(pd->_data));
            }
            return redis_message::null();
        });
    });
}

shared_ptr<abstract_command> mget::prepare(service::storage_proxy& proxy, const service::client_state& cs, request&& req)
{
    if (req._args_count < 1) {
        return unexpected::prepare(std::move(req._command), std::move(bytes { msg_syntax_err }) );
    }
    return seastar::make_shared<mget>(std::move(req._command), simple_objects_schema(proxy, cs.get_keyspace()), std::move(req._args));
}

future<redis_message> mget::execute(service::storage_proxy& proxy, db::consistency_level cl, db::timeout_clock::time_point now, const timeout_config& tc, service::client_state& cs)
{
    auto timeout = now + tc.read_timeout;
    return prefetch_simple(proxy, _schema, _keys, cl, timeout, cs).then([this, &proxy, cl, timeout, &cs] (auto pd) {
        if (pd && pd->has_data()) {
            //FIXME: if key was not exists, we should return nil message to client.
            auto&& values = boost::copy_range<std::vector<std::optional<bytes>>>(pd->data() | boost::adaptors::transformed([] (auto& p) { return std::optional<bytes>(std::move(p.second)); }));
            return redis_message::make(std::move(values));
        }
        return redis_message::null();
    });
}

}
}
