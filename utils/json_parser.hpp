#ifndef AURORA_JSON_PARSER_HPP
#define AURORA_JSON_PARSER_HPP

#include <stddef.h>

class JsonParser {
private:
    const char* raw_json_;

    // 内部微型辅助：查找字符，跳过转义字符
    const char* find_char(const char* str, char c) const {
        while (*str) {
            if (*str == '\\' && *(str + 1) != '\0') {
                str += 2; // 跳过转义序列
                continue;
            }
            if (*str == c) return str;
            str++;
        }
        return nullptr;
    }

    // 内部微型辅助：子串匹配
    const char* find_substr(const char* str, const char* sub) const {
        if (!*sub) return str;
        while (*str) {
            const char* p1 = str;
            const char* p2 = sub;
            while (*p1 && *p2 && *p1 == *p2) { p1++; p2++; }
            if (!*p2) return str;
            str++;
        }
        return nullptr;
    }

public:
    JsonParser(const char* json_string) : raw_json_(json_string) {}

    // ========================================================
    // 极速提取字符串值 (例如提取 "device_id":"aurora_watch_01")
    // ========================================================
    bool get_string(const char* key, char* out_buf, int max_len) const {
        if (!raw_json_ || !key || !out_buf || max_len <= 0) return false;

        // 1. 构造标准的带引号键名 (如 "\"device_id\"")
        char search_key[32];
        int i = 0;
        search_key[i++] = '"';
        while (*key && i < 30) search_key[i++] = *key++;
        search_key[i++] = '"';
        search_key[i] = '\0';

        // 2. 在原始缓冲中定位键名
        const char* key_pos = find_substr(raw_json_, search_key);
        if (!key_pos) return false;

        // 3. 寻找键名后面的冒号 ':'
        const char* colon_pos = find_char(key_pos, ':');
        if (!colon_pos) return false;

        // 4. 寻找目标字符串的起始引号 '"'
        const char* val_start = find_char(colon_pos, '"');
        if (!val_start) return false;
        val_start++; // 跳过起始引号

        // 5. 寻找目标字符串的结束引号 '"'
        const char* val_end = find_char(val_start, '"');
        if (!val_end) return false;

        // 6. 安全拷贝到输出缓冲区
        int val_len = val_end - val_start;
        if (val_len >= max_len) val_len = max_len - 1;
        
        // 拷贝时处理转义字符
        int out_idx = 0;
        for (int j = 0; j < val_len; j++) {
            if (val_start[j] == '\\' && j + 1 < val_len) {
                j++; // 跳过斜杠，直接拷贝被转义的字符
            }
            if (out_idx < max_len - 1) {
                out_buf[out_idx++] = val_start[j];
            }
        }
        out_buf[out_idx] = '\0';
        return true;
    }

    // ========================================================
    // 极速提取原始文本 (用于直接截取数组，如提取 ["display","touch"])
    // ========================================================
    bool get_raw_value(const char* key, char* out_buf, int max_len) const {
        if (!raw_json_ || !key || !out_buf || max_len <= 0) return false;

        char search_key[32];
        int i = 0; search_key[i++] = '"'; while (*key && i < 30) search_key[i++] = *key++; search_key[i++] = '"'; search_key[i] = '\0';

        const char* key_pos = find_substr(raw_json_, search_key);
        if (!key_pos) return false;

        const char* colon_pos = find_char(key_pos, ':');
        if (!colon_pos) return false;

        const char* val_start = colon_pos + 1;
        while (*val_start == ' ' || *val_start == '\t') val_start++; // 跳过空格
        
        // [安全加固] 防止畸形的末尾截断导致随后的 find_char 发生越界读取
        if (*val_start == '\0') return false;

        char end_char = (*val_start == '[') ? ']' : ',';
        const char* val_end = find_char(val_start + 1, end_char);
        if (!val_end) val_end = find_char(val_start + 1, '}'); // 兜底查对象结尾
        if (!val_end) return false;

        if (end_char == ']') val_end++; // 包含闭合括号

        int val_len = val_end - val_start;
        if (val_len >= max_len) val_len = max_len - 1;
        for (int j = 0; j < val_len; j++) out_buf[j] = val_start[j];
        out_buf[val_len] = '\0';
        return true;
    }
};

#endif
