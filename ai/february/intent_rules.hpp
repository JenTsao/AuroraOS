/**
 * @file intent_rules.hpp
 * @brief Static keyword → Intent rule table (extensible, no heap)
 */
#ifndef AURORA_FEBRUARY_INTENT_RULES_HPP
#define AURORA_FEBRUARY_INTENT_RULES_HPP

#include "types.hpp"

namespace aurora {
namespace february {

struct IntentRule {
    const char* keyword;
    IntentType  type;
    uint32_t    confidence_x1000;
    int32_t     param0;
    bool        requires_wake;
};

inline const IntentRule* default_intent_rules() {
    static const IntentRule kRules[] = {
        {"cancel dnd",     IntentType::SetDoNotDisturb, 850, 0, false},
        {"disable dnd",    IntentType::SetDoNotDisturb, 850, 0, false},
        {"dnd off",        IntentType::SetDoNotDisturb, 850, 0, false},
        {"clear focus",    IntentType::SetDoNotDisturb, 850, 0, false},
        {"do not disturb", IntentType::SetDoNotDisturb, 800, 1, false},
        {"focus mode",     IntentType::SetDoNotDisturb, 800, 1, false},
        {"dnd",            IntentType::SetDoNotDisturb, 800, 1, false},
        {"focus",          IntentType::SetDoNotDisturb, 750, 1, false},
        {"how am i",       IntentType::QueryStatus,     900, 0, false},
        {"status",         IntentType::QueryStatus,     900, 0, false},
        {"battery",        IntentType::QueryStatus,     850, 0, false},
        {"health",         IntentType::QueryHealth,     850, 0, false},
        {"heart",          IntentType::QueryHealth,     850, 0, false},
        {"help",           IntentType::Help,            950, 0, false},
        {"hello",          IntentType::Greeting,        900, 0, false},
        {"hi",             IntentType::Greeting,        850, 0, false},
        {nullptr,          IntentType::None,              0, 0, false},
    };
    return kRules;
}

}  // namespace february
}  // namespace aurora

#endif
