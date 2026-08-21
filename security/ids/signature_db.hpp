// =============================================================================
// security/ids/signature_db.hpp
//
// NIDS 攻击特征库：Aho-Corasick 多模式匹配 + Snort 规则子集
//
//   - AhoCorasick：稀疏 goto 表（节点 + 边链表）实现的 AC 自动机，
//     固定数组、零动态内存分配，单次扫描同时匹配多个内容特征
//   - SignatureDb：加载默认内容特征（shellcode / 注入 / 路径穿越 / CGI 等），
//     对包载荷做多模式匹配
//
// 设计原则（遵循 AGENTS.md）：
//   - 全静态存储，构建阶段一次完成 fail 链接与输出传播，搜索 O(n)
//   - 无浮点、无堆分配
// =============================================================================
#ifndef AURORA_IDS_SIGNATURE_DB_HPP
#define AURORA_IDS_SIGNATURE_DB_HPP

#include <stdint.h>
#include "alert_manager.hpp"

namespace aurora {
namespace ids {

// ---------------------------------------------------------------------------
// 特征规则
// ---------------------------------------------------------------------------
struct SignatureRule {
    const char* pattern; // 字节模式
    uint8_t len;         // 模式长度
    IdsSeverity severity;
    const char* message;
};

// ---------------------------------------------------------------------------
// Aho-Corasick 自动机（稀疏 goto 表）
// ---------------------------------------------------------------------------
class AhoCorasick {
public:
    static constexpr int kMaxNodes = 96;
    static constexpr int kMaxEdges = 192;
    static constexpr int kMaxPatterns = 16;
    static constexpr int kMaxPatternLen = 48;

    void reset() {
        node_count_ = 1;
        edge_count_ = 0;
        pattern_count_ = 0;
        nodes_[0].fail = 0;
        nodes_[0].output = -1;
        nodes_[0].edge_head = -1;
    }

    // 添加一个模式，返回模式 id（0 起始），失败返回 -1
    int add_pattern(const uint8_t* p, int len) {
        if (pattern_count_ >= kMaxPatterns || len <= 0 || len > kMaxPatternLen)
            return -1;

        const int id = pattern_count_;
        for (int i = 0; i < len; ++i)
            patterns_[id][i] = p[i];
        pattern_len_[id] = static_cast<uint8_t>(len);

        int32_t state = 0;
        for (int i = 0; i < len; ++i) {
            const int32_t g = goto_(state, p[i]);
            if (g == -1) {
                const int32_t child = new_node_();
                if (child < 0 || new_edge_(state, p[i], child) < 0)
                    return -1;
                state = child;
            } else {
                state = g;
            }
        }
        nodes_[state].output = id;
        ++pattern_count_;
        return id;
    }

    // 计算 fail 链接（BFS）并传播输出
    void build() {
        int head = 0;
        int tail = 0;

        for (int32_t e = nodes_[0].edge_head; e != -1; e = edges_[e].next_edge) {
            const int32_t child = edges_[e].next;
            nodes_[child].fail = 0;
            if (tail < kMaxNodes)
                queue_[tail++] = child;
        }

        while (head < tail) {
            const int32_t r = queue_[head++];
            for (int32_t e = nodes_[r].edge_head; e != -1; e = edges_[e].next_edge) {
                const uint8_t byte = edges_[e].byte;
                const int32_t child = edges_[e].next;
                if (tail < kMaxNodes)
                    queue_[tail++] = child;

                int32_t state = nodes_[r].fail;
                while (state != 0 && goto_(state, byte) == -1)
                    state = nodes_[state].fail;
                const int32_t f = goto_(state, byte);
                nodes_[child].fail = (f != -1 && f != child) ? f : 0;

                // 输出传播（字典链接）：继承 fail 链上已传播的输出
                if (nodes_[child].output == -1)
                    nodes_[child].output = nodes_[nodes_[child].fail].output;
            }
        }
    }

    // 搜索首个匹配模式 id，未命中返回 -1
    int search(const uint8_t* data, int len) const {
        int32_t state = 0;
        for (int i = 0; i < len; ++i) {
            const uint8_t b = data[i];
            while (state != 0 && goto_(state, b) == -1)
                state = nodes_[state].fail;
            const int32_t g = goto_(state, b);
            state = (g != -1) ? g : 0;
            if (nodes_[state].output != -1)
                return nodes_[state].output;
        }
        return -1;
    }

    int pattern_count() const {
        return pattern_count_;
    }

private:
    struct Node {
        int32_t fail;
        int32_t output;    // 模式 id，-1 表示无
        int32_t edge_head; // 边链表头，-1 表示无
    };
    struct Edge {
        uint8_t byte;
        int32_t next;      // 子节点下标
        int32_t next_edge; // 同节点下一条边，-1 表示尾
    };

    Node nodes_[kMaxNodes]{};
    Edge edges_[kMaxEdges]{};
    int node_count_ = 1;
    int edge_count_ = 0;
    uint8_t patterns_[kMaxPatterns][kMaxPatternLen]{};
    uint8_t pattern_len_[kMaxPatterns]{};
    int pattern_count_ = 0;
    int32_t queue_[kMaxNodes]{};

    int32_t goto_(int32_t state, uint8_t byte) const {
        for (int32_t e = nodes_[state].edge_head; e != -1; e = edges_[e].next_edge) {
            if (edges_[e].byte == byte)
                return edges_[e].next;
        }
        return -1;
    }

    int32_t new_node_() {
        if (node_count_ >= kMaxNodes)
            return -1;
        const int idx = node_count_++;
        nodes_[idx].fail = 0;
        nodes_[idx].output = -1;
        nodes_[idx].edge_head = -1;
        return idx;
    }

    int32_t new_edge_(int32_t from, uint8_t byte, int32_t to) {
        if (edge_count_ >= kMaxEdges)
            return -1;
        const int idx = edge_count_++;
        edges_[idx].byte = byte;
        edges_[idx].next = to;
        edges_[idx].next_edge = nodes_[from].edge_head;
        nodes_[from].edge_head = idx;
        return idx;
    }
};

// ---------------------------------------------------------------------------
// SignatureDb：Snort 规则子集（内容匹配）
// ---------------------------------------------------------------------------
class SignatureDb {
public:
    static constexpr int kMaxSignatures = 8;

    void init() {
        rules_ = default_rules_();
        rule_count_ = kDefaultCount;
        ac_.reset();
        for (int i = 0; i < rule_count_; ++i) {
            const SignatureRule& r = rules_[i];
            ac_.add_pattern(reinterpret_cast<const uint8_t*>(r.pattern), r.len);
        }
        ac_.build();
    }

    // 对载荷做多模式匹配，返回命中的规则下标（-1 未命中）
    int match(const uint8_t* payload, int len) const {
        if (len <= 0)
            return -1;
        return ac_.search(payload, len);
    }

    const SignatureRule& rule(int idx) const {
        return rules_[idx];
    }

    int rule_count() const {
        return rule_count_;
    }

private:
    static constexpr int kDefaultCount = 6;

    static const SignatureRule* default_rules_() {
        static const SignatureRule rules[] = {
            {"\x90\x90\x90\x90\x90", 5, IdsSeverity::High, "NOP sled / shellcode"},
            {"/bin/sh", 7, IdsSeverity::Critical, "shell command injection"},
            {"../../", 5, IdsSeverity::Medium, "path traversal"},
            {"union select", 12, IdsSeverity::High, "SQL injection"},
            {"USER root", 9, IdsSeverity::Low, "FTP root login"},
            {"GET /cgi-bin", 12, IdsSeverity::Low, "CGI scan"},
        };
        return rules;
    }

    AhoCorasick ac_;
    const SignatureRule* rules_ = nullptr;
    int rule_count_ = 0;
};

} // namespace ids
} // namespace aurora

#endif // AURORA_IDS_SIGNATURE_DB_HPP
