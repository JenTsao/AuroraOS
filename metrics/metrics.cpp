#include "metrics.hpp"

static LatencyRecorder g_recorders[METRIC_MAX];
static PowerProfiler g_power_profiler;

static uint32_t g_net_drops = 0;
static uint32_t g_softbus_registers = 0;
static uint32_t g_heap_defrags = 0;
static bool g_is_active = false;

void Metrics::init() {
    for (int i = 0; i < METRIC_MAX; i++) {
        g_recorders[i].reset();
    }
    g_power_profiler.reset();
    g_net_drops = 0;
    g_softbus_registers = 0;
    g_heap_defrags = 0;
    g_is_active = false;
}

void Metrics::record(MetricId id, uint32_t value) {
    if (!g_is_active || id >= METRIC_MAX) return;
    g_recorders[id].record(value);
}

LatencyRecorder& Metrics::get_recorder(MetricId id) {
    if (id >= METRIC_MAX) return g_recorders[0];
    return g_recorders[id];
}

PowerProfiler& Metrics::get_power_profiler() {
    return g_power_profiler;
}

void Metrics::inc_net_drop() {
    if (g_is_active) g_net_drops++;
}

void Metrics::inc_softbus_register() {
    if (g_is_active) g_softbus_registers++;
}

void Metrics::inc_heap_defrag() {
    if (g_is_active) g_heap_defrags++;
}

uint32_t Metrics::get_net_drops() { return g_net_drops; }
uint32_t Metrics::get_softbus_registers() { return g_softbus_registers; }
uint32_t Metrics::get_heap_defrags() { return g_heap_defrags; }

void Metrics::start_measurement() {
    init();
    g_is_active = true;
}

void Metrics::stop_measurement() {
    g_is_active = false;
}

bool Metrics::is_active() {
    return g_is_active;
}
