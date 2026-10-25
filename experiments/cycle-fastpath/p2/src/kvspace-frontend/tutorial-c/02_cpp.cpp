/*
 * 02_cpp — C++ RAII 封装：extern "C" 直接调用 libkvspace dispatch 前端
 *
 * 编译: g++ -std=c++17 02_cpp.cpp -lkvspace
 */

extern "C" {
    #include "kvspace/kvspace.h"
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

struct KV {
    void *kv;
    std::string path;

    KV(const char *p) : path(p) {
        ::unlink(p);
        std::string dsn = "shm://" + path;
        kv = kvspaceConnect(dsn.c_str());
        if (!kv) throw std::runtime_error("connect failed");
    }
    ~KV() { kvspaceClose(kv); ::unlink(path.c_str()); }

    // 写即构造：解预编码 XValue 取 kindexpr+body，就地/新位置二选一原语，往返回指针写 body。
    void set(const char *key, const uint8_t *val, uint32_t len) {
        kvspaceHead_t h;
        if (kvspaceDecodeHead(val, len, &h) != 0) return;
        uint32_t bl = h.body_len > 0 ? (uint32_t)h.body_len : 0;
        uint8_t *dst = nullptr; char err[256];
        if (kvspaceWriteInPlace(kv, key, 1, bl, &dst, err, sizeof(err)) != 0)
            kvspaceWriteNewPlace(kv, key, (const char *)h.kindexpr, bl, &dst, err, sizeof(err));
        if (bl && dst) memcpy(dst, val + h.body_offset, bl);
    }
    // 借用读：out 指向 kvspace 常驻空间，拷进 std::string 后即失效，不 free。
    bool get(const char *key, std::string &kind, std::string &raw) {
        uint8_t *out = nullptr; uint32_t len = 0;
        kvspaceGet(kv, key, 0, &out, &len);
        if (!out || len == 0) return false;
        kvspaceHead_t h;
        kvspaceDecodeHead(out, len, &h);
        const char *k = (const char *)h.kindexpr;
        if (k[0] == '*' || k[0] == '@') k++;
        if (k[0] == '[') { const char *e = strchr(k, ']'); if (e) k = e + 1; }
        kind = k;
        raw.assign((const char *)(out + h.body_offset), h.body_len > 0 ? h.body_len : 0);
        return true;
    }
    void del(const char *key) {
        const char *keys[1] = { key };
        kvspaceDel(kv, keys, 1, nullptr, 0);
    }
    void deltree(const char *p) { kvspaceDelTree(kv, p, nullptr, 0); }
    void mkindex(const char *p) { kvspaceMkindex(kv, p, nullptr, 0); }
    // 前缀枚举：ListLen 定计数 + 逐 idx ListAt 借用取名（读出即拷入 vector）。
    std::vector<std::string> list(const char *prefix) {
        std::vector<std::string> r;
        int32_t count = 0;
        if (kvspaceListLen(kv, prefix, 0, 1, &count) != 0 || count <= 0) return r;
        for (int32_t i = 0; i < count; i++) {
            uint8_t *nm = nullptr; uint32_t nl = 0;
            if (kvspaceListAt(kv, prefix, 0, 1, i, &nm, &nl) == 0 && nm)
                r.push_back(std::string((const char *)nm, nl));
        }
        return r;
    }
};

int main() {
    auto xv_int = [](int64_t v) {
        uint8_t *b; uint32_t n; kvspaceNewInt64(v, &b, &n);
        return std::pair<uint8_t *, uint32_t>{b, n};
    };
    auto xv_str = [](const char *s) {
        uint8_t *b; uint32_t n; kvspaceNewChar((const uint8_t *)s, (uint32_t)strlen(s), &b, &n);
        return std::pair<uint8_t *, uint32_t>{b, n};
    };

    KV kv("/tmp/kvspace_cpp.shm");
    kv.mkindex("/cpp/");

    auto [vi, li] = xv_int(42);
    kv.set("/cpp/a", vi, li);
    free(vi);

    auto [vs, ls] = xv_str("hello");
    kv.set("/cpp/b", vs, ls);
    free(vs);

    std::string kind, raw;
    if (kv.get("/cpp/a", kind, raw))
        printf("/cpp/a  kind=%s val=%ld\n", kind.c_str(), *(const int64_t *)raw.data());
    if (kv.get("/cpp/b", kind, raw))
        printf("/cpp/b  kind=%s val=%.*s\n", kind.c_str(), (int)raw.size(), raw.data());

    auto ns = kv.list("/cpp/");
    printf("list /cpp/:");
    for (auto &n : ns) printf(" %s", n.c_str());
    printf(" (count=%zu)\n", ns.size());

    kv.deltree("/cpp/");
    printf("deltree ok, get /cpp/a: %s\n", kv.get("/cpp/a", kind, raw) ? "EXISTS" : "NULL");

    printf("PASS cpp\n");
    return 0;
}
