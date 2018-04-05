/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2018 Couchbase, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#ifndef LCB_TRACING_INTERNAL_H
#define LCB_TRACING_INTERNAL_H

#ifdef LCB_TRACING
#include <libcouchbase/tracing.h>
#include "rnd.h"

#ifdef __cplusplus

#include <queue>

namespace lcb
{
namespace trace
{

class Span
{
  public:
    Span(lcbtrace_TRACER *tracer, const char *opname, uint64_t start, lcbtrace_REF_TYPE ref, lcbtrace_SPAN *other);

    void finish(uint64_t finish);
    uint64_t duration()
    {
        return m_finish - m_start;
    }

    template < typename T > void add_tag(const char *name, T value);

    lcbtrace_TRACER *m_tracer;
    std::string m_opname;
    uint64_t m_span_id;
    uint64_t m_start;
    uint64_t m_finish;
    bool m_orphaned;
    Json::Value tags;
    Span *m_parent;
};

struct ReportedSpan {
    uint64_t duration;
    std::string payload;

    bool operator<(const ReportedSpan &rhs) const
    {
        return duration < rhs.duration;
    }
};

template < typename T > class FixedQueue
{
  public:
    explicit FixedQueue(size_t capacity) : m_capacity(capacity) {}

    void push(T item)
    {
        if (m_items.size() < m_capacity) {
            m_items.push_back(item);
            std::push_heap(m_items.begin(), m_items.end());
        } else {
            std::sort_heap(m_items.begin(), m_items.end());
            if (m_items.front() < item) {
                m_items[0] = item;
            }
            std::make_heap(m_items.begin(), m_items.end());
        }
    }

    size_t size()
    {
        return m_items.size();
    }

    bool empty()
    {
        return m_items.empty();
    }

    void clear()
    {
        m_items.clear();
    }

    std::vector< T > &get_sorted()
    {
        std::sort_heap(m_items.begin(), m_items.end());
        return m_items;
    }

  private:
    size_t m_capacity;
    std::vector< T > m_items;
};

class ThresholdLoggingTracer
{
    lcbtrace_TRACER *m_wrapper;
    lcb_settings *m_settings;

    FixedQueue< ReportedSpan > m_orphans;
    FixedQueue< ReportedSpan > m_threshold;

    void flush_queue(FixedQueue< ReportedSpan > &queue, const char *message);
    ReportedSpan convert(lcbtrace_SPAN *span);

  public:
    ThresholdLoggingTracer(lcb_t instance);

    lcbtrace_TRACER *wrap();
    void add_orphan(lcbtrace_SPAN *span);
    void check_threshold(lcbtrace_SPAN *span);

    void flush_orphans();
    void flush_threshold();

    lcb::io::Timer< ThresholdLoggingTracer, &ThresholdLoggingTracer::flush_orphans > m_oflush;
    lcb::io::Timer< ThresholdLoggingTracer, &ThresholdLoggingTracer::flush_threshold > m_tflush;
};

} // namespace trace
} // namespace lcb

extern "C" {
#endif
LCB_INTERNAL_API
void lcbtrace_span_add_system_tags(lcbtrace_SPAN *span, lcb_settings *settings, const char *service);
LCB_INTERNAL_API
void lcbtrace_span_set_parent(lcbtrace_SPAN *span, lcbtrace_SPAN *parent);
LCB_INTERNAL_API
void lcbtrace_span_set_orphaned(lcbtrace_SPAN *span, int val);

#define LCBTRACE_KV_START(settings, cmd, operation_name, opaque, outspan)                                              \
    if ((settings)->tracer) {                                                                                          \
        lcbtrace_REF ref;                                                                                              \
        char opid[20] = {};                                                                                            \
        snprintf(opid, sizeof(opid), "0x%x", (int)opaque);                                                             \
        ref.type = LCBTRACE_REF_CHILD_OF;                                                                              \
        ref.span = (cmd->_hashkey.type & LCB_KV_TRACESPAN) ? (lcbtrace_SPAN *)cmd->_hashkey.contig.bytes : NULL;       \
        outspan = lcbtrace_span_start((settings)->tracer, operation_name, LCBTRACE_NOW, &ref);                         \
        lcbtrace_span_add_tag_str(outspan, LCBTRACE_TAG_OPERATION_ID, opid);                                           \
        lcbtrace_span_add_system_tags(outspan, (settings), LCBTRACE_TAG_SERVICE_KV);                                   \
    }

#define LCBTRACE_KV_FINISH(pipeline, request, response)                                                                \
    do {                                                                                                               \
        lcbtrace_SPAN *span = MCREQ_PKT_RDATA(request)->span;                                                          \
        if (span) {                                                                                                    \
            lcbtrace_span_add_tag_uint64(span, LCBTRACE_TAG_PEER_LATENCY, (response)->duration());                     \
            lcb::Server *server = static_cast< lcb::Server * >(pipeline);                                              \
            const lcb_host_t &remote = server->get_host();                                                             \
            std::string hh;                                                                                            \
            if (remote.ipv6) {                                                                                         \
                hh.append("[").append(remote.host).append("]:").append(remote.port);                                   \
            } else {                                                                                                   \
                hh.append(remote.host).append(":").append(remote.port);                                                \
            }                                                                                                          \
            lcbtrace_span_add_tag_str(span, LCBTRACE_TAG_PEER_ADDRESS, hh.c_str());                                    \
            lcbio_CTX *ctx = server->connctx;                                                                          \
            if (ctx) {                                                                                                 \
                char local_id[34] = {};                                                                                \
                snprintf(local_id, sizeof(local_id), "%016" PRIx64 "/%016" PRIx64,                                     \
                         (lcb_U64)server->get_settings()->iid, ctx->sock->id);                                         \
                lcbtrace_span_add_tag_str(span, LCBTRACE_TAG_LOCAL_ID, local_id);                                      \
                lcbtrace_span_add_tag_str(span, LCBTRACE_TAG_LOCAL_ADDRESS,                                            \
                                          lcbio__inet_ntop(&ctx->sock->info->sa_local).c_str());                       \
            }                                                                                                          \
            lcbtrace_span_finish(span, LCBTRACE_NOW);                                                                  \
            MCREQ_PKT_RDATA(request)->span = NULL;                                                                     \
        }                                                                                                              \
    } while (0);

#ifdef __cplusplus
}
#endif

#else

#define LCBTRACE_KV_START(settings, cmd, operation_name, opaque, outspan)
#define LCBTRACE_KV_FINISH(pipeline, request, response)

#endif /* LCB_TRACING */

#endif /* LCB_TRACING_INTERNAL_H */
