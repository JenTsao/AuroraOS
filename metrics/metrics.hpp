#ifndef AURORA_METRICS_HPP
#define AURORA_METRICS_HPP

#include <stdint.h>
#include "latency_recorder.hpp"
#include "power_profiler.hpp"

enum MetricId {
    METRIC_IRQ_LATENCY,
    METRIC_CTX_SWITCH,
    METRIC_HEAP_64B,
    METRIC_DIRTY_RATIO,
    METRIC_MAX
};

class Metrics {
public:
    static void init();
    static void record(MetricId id, uint32_t value);
    static LatencyRecorder& get_recorder(MetricId id);
    static PowerProfiler& get_power_profiler();
    
    static void inc_net_drop();
    static void inc_softbus_register();
    static void inc_heap_defrag();
    
    static uint32_t get_net_drops();
    static uint32_t get_softbus_registers();
    static uint32_t get_heap_defrags();
    
    static void start_measurement();
    static void stop_measurement();
    static bool is_active();
};

#endif
