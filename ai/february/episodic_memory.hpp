#ifndef AURORA_FEBRUARY_EPISODIC_MEMORY_HPP
#define AURORA_FEBRUARY_EPISODIC_MEMORY_HPP

#include "types.hpp"
#include "config.hpp"
#include "crit.hpp"
#include "string_util.hpp"

namespace aurora {
namespace february {

#if FEBRUARY_ENABLE_EPISODIC_MEMORY

#ifndef FEBRUARY_EPISODIC_MAX_HABITS
#define FEBRUARY_EPISODIC_MAX_HABITS 16
#endif

enum class HabitTrigger : uint8_t {
    None = 0, WristRaise, HighHeartRate, EnterRoom, LowBattery, VoiceCommand, Custom
};

enum class HabitAction : uint8_t {
    None = 0, CheckTimeOnly, DimDisplay, NotifyQuiet, RouteLightNearest, SuppressBright, Custom
};

struct HabitRule {
    char         key[16] = {};
    uint8_t      hour_start = 0;
    uint8_t      hour_end   = 23;
    HabitTrigger trigger    = HabitTrigger::None;
    HabitAction  action     = HabitAction::None;
    ConfidenceQ8 confidence_q8 = 0;
    uint8_t      weekday_mask = 0x7F;
    uint16_t     hit_count  = 0;
};

struct EpisodicKvOps {
    int (*get)(const char* key, void* buf, unsigned max_len, void* user) = nullptr;
    int (*put)(const char* key, const void* buf, unsigned len, void* user) = nullptr;
    int (*del)(const char* key, void* user) = nullptr;
    void* user = nullptr;
};

class EpisodicMemory {
public:
    static EpisodicMemory& instance() {
        static EpisodicMemory em;
        return em;
    }

    void set_kv(const EpisodicKvOps& ops) {
        FebruaryCrit::Guard g;
        kv_ = ops;
    }

    bool upsert(const HabitRule& rule) {
        if (!rule.key[0] || rule.trigger == HabitTrigger::None) return false;
        FebruaryCrit::Guard g;
        int idx = find_key_unlocked(rule.key);
        if (idx < 0) {
            idx = alloc_unlocked();
            if (idx < 0) return false;
        }
        habits_[idx] = rule;
        dirty_ = true;
        return true;
    }

    bool remove(const char* key) {
        if (!key || !key[0]) return false;
        FebruaryCrit::Guard g;
        int idx = find_key_unlocked(key);
        if (idx < 0) return false;
        habits_[idx] = HabitRule{};
        dirty_ = true;
        return true;
    }

    bool reinforce_habit(const char* key, uint8_t delta_conf = 10) {
        if (!key || !key[0]) return false;
        FebruaryCrit::Guard g;
        int idx = find_key_unlocked(key);
        if (idx < 0) return false;
        uint16_t new_c = habits_[idx].confidence_q8 + delta_conf;
        habits_[idx].confidence_q8 = (new_c > 255) ? 255 : static_cast<ConfidenceQ8>(new_c);
        habits_[idx].hit_count++;
        dirty_ = true;
        return true;
    }

    bool match(uint8_t hour, uint8_t weekday, HabitTrigger trigger, HabitRule* out) const {
        FebruaryCrit::Guard g;
        int best = -1;
        ConfidenceQ8 best_c = 0;
        for (unsigned i = 0; i < FEBRUARY_EPISODIC_MAX_HABITS; ++i) {
            const HabitRule& h = habits_[i];
            if (h.trigger != trigger || !h.key[0]) continue;
            if (!hour_in_range(hour, h.hour_start, h.hour_end)) continue;
            if (weekday < 7 && ((h.weekday_mask & (1u << weekday)) == 0)) continue;
            if (h.confidence_q8 >= best_c) {
                best_c = h.confidence_q8;
                best = static_cast<int>(i);
            }
        }
        if (best < 0) return false;
        if (out) *out = habits_[best];
        return true;
    }

    void seed_defaults() {
        HabitRule night{};
        copy_cstr(night.key, sizeof(night.key), "night_wrist");
        night.hour_start = 22;
        night.hour_end = 5;
        night.trigger = HabitTrigger::WristRaise;
        night.action = HabitAction::CheckTimeOnly;
        night.confidence_q8 = 200;
        night.weekday_mask = 0x7F;
        upsert(night);
    }

    bool flush() {
        FebruaryCrit::Guard g;
        if (!kv_.put) { dirty_ = false; return true; }
        for (unsigned i = 0; i < FEBRUARY_EPISODIC_MAX_HABITS; ++i) {
            if (!habits_[i].key[0]) continue;
            char k[24];
            copy_cstr(k, sizeof(k), "feb.hab.");
            unsigned n = 0; while (k[n]) ++n;
            for (unsigned j = 0; habits_[i].key[j] && n + 1 < sizeof(k); ++j) k[n++] = habits_[i].key[j];
            k[n] = '\0';
            if (kv_.put(k, &habits_[i], sizeof(HabitRule), kv_.user) != 0) return false;
        }
        dirty_ = false;
        return true;
    }

    bool load_from_kv(const char* const* keys, unsigned nkeys) {
        if (!kv_.get || !keys) return false;
        FebruaryCrit::Guard g;
        for (unsigned i = 0; i < nkeys && i < FEBRUARY_EPISODIC_MAX_HABITS; ++i) {
            HabitRule h{};
            char k[24];
            copy_cstr(k, sizeof(k), "feb.hab.");
            unsigned n = 0; while (k[n]) ++n;
            for (unsigned j = 0; keys[i][j] && n + 1 < sizeof(k); ++j) k[n++] = keys[i][j];
            k[n] = '\0';
            const int rd = kv_.get(k, &h, sizeof(h), kv_.user);
            if (rd == static_cast<int>(sizeof(HabitRule)) && h.key[0]) {
                int slot = find_key_unlocked(h.key);
                if (slot < 0) slot = alloc_unlocked();
                if (slot >= 0) habits_[slot] = h;
            }
        }
        dirty_ = false;
        return true;
    }

    unsigned count() const {
        FebruaryCrit::Guard g;
        unsigned n = 0;
        for (unsigned i = 0; i < FEBRUARY_EPISODIC_MAX_HABITS; ++i)
            if (habits_[i].key[0]) ++n;
        return n;
    }

    bool dirty() const { return dirty_; }

    void clear() {
        FebruaryCrit::Guard g;
        for (unsigned i = 0; i < FEBRUARY_EPISODIC_MAX_HABITS; ++i) habits_[i] = HabitRule{};
        dirty_ = false;
    }

    static EpisodicKvOps make_ram_kv() {
        EpisodicKvOps ops;
        ops.get = &ram_get;
        ops.put = &ram_put;
        ops.del = &ram_del;
        ops.user = &ram_store();
        return ops;
    }

private:
    EpisodicMemory() = default;

    static bool hour_in_range(uint8_t hour, uint8_t start, uint8_t end) {
        if (start <= end) return hour >= start && hour <= end;
        return hour >= start || hour <= end;
    }

    int find_key_unlocked(const char* key) const {
        for (unsigned i = 0; i < FEBRUARY_EPISODIC_MAX_HABITS; ++i)
            if (habits_[i].key[0] && str_eq(habits_[i].key, key)) return static_cast<int>(i);
        return -1;
    }

    int alloc_unlocked() {
        for (unsigned i = 0; i < FEBRUARY_EPISODIC_MAX_HABITS; ++i)
            if (!habits_[i].key[0]) return static_cast<int>(i);
        return -1;
    }

    static bool str_eq(const char* a, const char* b) {
        if (!a || !b) return false;
        while (*a && *b && *a == *b) { ++a; ++b; }
        return *a == *b;
    }

    struct RamEntry { char key[28] = {}; HabitRule value{}; bool used = false; };
    static constexpr unsigned kRamSlots = FEBRUARY_EPISODIC_MAX_HABITS;
    struct RamStore { RamEntry entries[kRamSlots]{}; };
    static RamStore& ram_store() { static RamStore s; return s; }

    static int ram_get(const char* key, void* buf, unsigned max_len, void* user) {
        auto* store = static_cast<RamStore*>(user);
        if (!store || !key || !buf || max_len < sizeof(HabitRule)) return -1;
        for (unsigned i = 0; i < kRamSlots; ++i) {
            if (store->entries[i].used && str_eq(store->entries[i].key, key)) {
                *static_cast<HabitRule*>(buf) = store->entries[i].value;
                return static_cast<int>(sizeof(HabitRule));
            }
        }
        return -1;
    }

    static int ram_put(const char* key, const void* buf, unsigned len, void* user) {
        auto* store = static_cast<RamStore*>(user);
        if (!store || !key || !buf || len != sizeof(HabitRule)) return -1;
        int free_i = -1;
        for (unsigned i = 0; i < kRamSlots; ++i) {
            if (store->entries[i].used && str_eq(store->entries[i].key, key)) {
                store->entries[i].value = *static_cast<const HabitRule*>(buf);
                return 0;
            }
            if (!store->entries[i].used && free_i < 0) free_i = static_cast<int>(i);
        }
        if (free_i < 0) return -1;
        copy_cstr(store->entries[free_i].key, sizeof(store->entries[free_i].key), key);
        store->entries[free_i].value = *static_cast<const HabitRule*>(buf);
        store->entries[free_i].used = true;
        return 0;
    }

    static int ram_del(const char* key, void* user) {
        auto* store = static_cast<RamStore*>(user);
        if (!store || !key) return -1;
        for (unsigned i = 0; i < kRamSlots; ++i) {
            if (store->entries[i].used && str_eq(store->entries[i].key, key)) {
                store->entries[i] = RamEntry{};
                return 0;
            }
        }
        return -1;
    }

    HabitRule      habits_[FEBRUARY_EPISODIC_MAX_HABITS]{};
    EpisodicKvOps  kv_{};
    bool           dirty_ = false;
};

#else

struct HabitRule {};
enum class HabitTrigger : uint8_t { None = 0 };
enum class HabitAction : uint8_t { None = 0 };

class EpisodicMemory {
public:
    static EpisodicMemory& instance() { static EpisodicMemory em; return em; }
    bool upsert(const HabitRule&) { return false; }
    bool match(uint8_t, uint8_t, HabitTrigger, HabitRule*) const { return false; }
    void seed_defaults() {}
    bool flush() { return true; }
    unsigned count() const { return 0; }
    void clear() {}
};

#endif

}  // namespace february
}  // namespace aurora

#endif  // AURORA_FEBRUARY_EPISODIC_MEMORY_HPP
