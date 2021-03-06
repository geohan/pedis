#include "redis/commands/zrange.hh"
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

template<typename CommandType>
shared_ptr<abstract_command> prepare_impl(service::storage_proxy& proxy, const service::client_state& cs, request&& req)
{
    if (req._args_count < 3) {
        return unexpected::prepare(std::move(req._command), std::move(bytes { msg_syntax_err }) );
    }
    bool with_scores = false;
    if (req._args_count > 3) {
        if (req._args[3] == bytes("withscores")) {
            with_scores = true;
        }
    }
    auto begin = bytes2long(req._args[1]);
    auto end = bytes2long(req._args[2]);
    return seastar::make_shared<CommandType>(std::move(req._command), zsets_schema(proxy, cs.get_keyspace()), std::move(req._args[0]), begin, end, with_scores);
}

shared_ptr<abstract_command> zrange::prepare(service::storage_proxy& proxy, const service::client_state& cs, request&& req) {
    return prepare_impl<zrange>(proxy, cs, std::move(req));
}

shared_ptr<abstract_command> zrevrange::prepare(service::storage_proxy& proxy, const service::client_state& cs, request&& req) {
    return prepare_impl<zrevrange>(proxy, cs, std::move(req));
}

future<redis_message> zrange::execute_impl(service::storage_proxy& proxy, db::consistency_level cl, db::timeout_clock::time_point now, const timeout_config& tc, service::client_state& cs, bool reversed)
{
    auto timeout = now + tc.read_timeout;
    return prefetch_map(proxy, _schema, _key, fetch_options::all, cl, timeout, cs).then([this, &proxy, cl, timeout, &cs, reversed] (auto pd) {
        std::vector<std::optional<bytes>> results; 
        if (_begin < 0) _begin = 0;
        if (pd && pd->has_data()) {
            while (_end < 0 && pd->data().size() > 0) _end += static_cast<long>(pd->data().size());
            if (static_cast<size_t>(_end) >= pd->data().size()) _end = static_cast<long>(pd->data().size()) - 1;
            if (_begin <= _end) {
                auto&& result_scores = boost::copy_range<std::vector<std::pair<std::optional<bytes>, double>>> (pd->data() | boost::adaptors::transformed([] (auto& e) {
                    return std::move(std::pair<std::optional<bytes>, double>(std::move(e.first), bytes2double(*(e.second))));
                }));
                if (reversed) {
                    std::sort(result_scores.begin(), result_scores.end(), [] (auto& e1, auto& e2) { return e1.second > e2.second; });
                } else {
                    std::sort(result_scores.begin(), result_scores.end(), [] (auto& e1, auto& e2) { return e1.second < e2.second; });
                }
                if (static_cast<size_t>(_end) < result_scores.size()) {
                    result_scores.erase(result_scores.begin() + static_cast<size_t>(_end), result_scores.end());
                }
                if (_begin > 0) {
                    result_scores.erase(result_scores.begin(), result_scores.begin() + static_cast<size_t>(_begin));
                }
                for (auto&& e : result_scores) {
                    results.emplace_back(std::move(e.first));
                    if (_with_scores) {
                        results.emplace_back(std::move(double2bytes(e.second)));
                    }
                }
            }
        }
        return redis_message::make(std::move(results));
    });
}

}
}
