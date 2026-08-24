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
        free(v);
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
    // xvalue encode (manual TLV)
    auto xv_int = [](int64_t v) -> std::pair<std::vector<uint8_t>, int32_t> {
        const char *k = "int64"; int32_t kl = 5;
        std::vector<uint8_t> buf(1+kl+8+8);
        buf[0] = kl; memcpy(&buf[1], k, kl);
        int32_t al=1, rl=8;
        memcpy(&buf[1+kl], &al, 4); memcpy(&buf[1+kl+4], &rl, 4);
        memcpy(&buf[1+kl+8], &v, 8);
        return {buf, (int32_t)buf.size()};
    };
    auto xv_str = [](const char *s) -> std::pair<std::vector<uint8_t>, int32_t> {
        int32_t sl = strlen(s), kl = 6;
        std::vector<uint8_t> buf(1+kl+8+sl);
        buf[0] = kl; memcpy(&buf[1], "string", kl);
        int32_t al=1;
        memcpy(&buf[1+kl], &al, 4); memcpy(&buf[1+kl+4], &sl, 4);
        memcpy(&buf[1+kl+8], s, sl);
        return {buf, (int32_t)buf.size()};
    };

    KV kv("/tmp/kvspace_cpp.shm");
    kv.mkindex("/cpp/");

    auto [vi, li] = xv_int(42);
    kv.set("/cpp/a", vi.data(), li);

    auto [vs, ls] = xv_str("hello");
    kv.set("/cpp/b", vs.data(), ls);

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
