/*
 * 02_cpp — C++ example: extern "C" 直接调用 libkvspace-c.so
 *
 * 编译: g++ -std=c++17 02_cpp.cpp -I../include -L../build -lkvspace-c
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

// simple RAII wrapper
struct KV {
    kvspace_t *kv;
    std::string path;

    KV(const char *p, size_t sz = 32768) : path(p) {
        ::unlink(p);
        kv = kvspaceShmOpen(p, sz);
        if (!kv) throw std::runtime_error("open failed");
    }
    ~KV() { kvspaceShmClose(kv); ::unlink(path.c_str()); }

    void set(const char *key, const uint8_t *val, int32_t len) { kvspaceShmSet(kv, key, val, len); }
    bool get(const char *key, std::string &kind, std::string &raw) {
        int32_t len; uint8_t *v = kvspaceShmGet(kv, key, 1, &len);
        if (!v) return false;
        xvalue_head_t h = kvspaceXvalueDecodeHead(v, len);
        kind.assign(h.kind, h.kind_len);
        raw.assign((const char *)h.raw, h.raw_len);
        return true;
    }
    void del(const char *key) { kvspaceShmDel(kv, key); }
    void deltree(const char *p) { kvspaceShmDeltree(kv, p); }
    void mkindex(const char *p) { kvspaceShmMkindex(kv, p); }
    std::vector<std::string> list(const char *prefix) {
        char **ns; int32_t nc;
        kvspaceShmList(kv, prefix, false, 1, &ns, &nc);
        std::vector<std::string> r;
        for (int i = 0; i < nc; i++) r.push_back(ns[i]);
        for (int i = 0; i < nc; i++) free(ns[i]); free(ns);
        return r;
    }
};

int main() {
    // xvalue encode（用库构造函数，非手工 TLV）
    auto xv_int = [](int64_t v) {
        uint8_t *b; int32_t n = kvspaceXvalueNewInt64(&v, 1, &b);
        return std::pair<uint8_t *, int32_t>{b, n};
    };
    auto xv_str = [](const char *s) {
        uint8_t *b; int32_t n = kvspaceXvalueNewCharUtf8(s, &b);
        return std::pair<uint8_t *, int32_t>{b, n};
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
    if (kv.get("/cpp/a", kind, raw)) {
        printf("/cpp/a  kind=%s val=%ld\n", kind.c_str(), *(int64_t*)raw.data());
    }
    if (kv.get("/cpp/b", kind, raw)) {
        printf("/cpp/b  kind=%s val=%.*s\n", kind.c_str(), (int)raw.size(), raw.data());
    }

    auto ns = kv.list("/cpp/");
    printf("list /cpp/:");
    for (auto &n : ns) printf(" %s", n.c_str());
    printf(" (count=%zu)\n", ns.size());

    kv.deltree("/cpp/");
    printf("deltree ok, get /cpp/a: %s\n", kv.get("/cpp/a", kind, raw) ? "EXISTS" : "NULL");

    printf("PASS cpp\n");
    return 0;
}
