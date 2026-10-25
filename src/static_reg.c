/* static_reg.c — 静态单二进制：把 shm 后端的 ABI 符号表注册给前端 dispatch。
 * 仅 KVSPACE_STATIC_BUILD 编译。符号名用公共名（与 dlsym 同名），值指向
 * static_prefix.h 改名后的 kvspaceC*。scheme 精确匹配 "shm"。 */
#include <kvspace/kvspace.h>
#include <kvspace/kvspace_static.h>
#include "xvalue_head.h"
#include "xvalue_meta.h"

static const kvspace_static_sym_t g_syms[] = {
    { "kvspaceClear", (void *)(kvspaceClear) },
    { "kvspaceClose", (void *)(kvspaceClose) },
    { "kvspaceConnect", (void *)(kvspaceConnect) },
    { "kvspaceCp", (void *)(kvspaceCp) },
    { "kvspaceCpList", (void *)(kvspaceCpList) },
    { "kvspaceCpTree", (void *)(kvspaceCpTree) },
    { "kvspaceDecodeHead", (void *)(kvspaceDecodeHead) },
    { "kvspaceDel", (void *)(kvspaceDel) },
    { "kvspaceDelTree", (void *)(kvspaceDelTree) },
    { "kvspaceGet", (void *)(kvspaceGet) },
    { "kvspaceGetByRef", (void *)(kvspaceGetByRef) },
    { "kvspaceGetHead", (void *)(kvspaceGetHead) },
    { "kvspaceGetPart", (void *)(kvspaceGetPart) },
    { "kvspaceListAt", (void *)(kvspaceListAt) },
    { "kvspaceListLen", (void *)(kvspaceListLen) },
    { "kvspaceMetaDecode", (void *)(kvspaceMetaDecode) },
    { "kvspaceMetaEncode", (void *)(kvspaceMetaEncode) },
    { "kvspaceMetaKey", (void *)(kvspaceMetaKey) },
    { "kvspaceMkindex", (void *)(kvspaceMkindex) },
    { "kvspaceMkindexExt", (void *)(kvspaceMkindexExt) },
    { "kvspaceNewBool", (void *)(kvspaceNewBool) },
    { "kvspaceNewChar", (void *)(kvspaceNewChar) },
    { "kvspaceNewFloat64", (void *)(kvspaceNewFloat64) },
    { "kvspaceNewInt64", (void *)(kvspaceNewInt64) },
    { "kvspaceNewPtr", (void *)(kvspaceNewPtr) },
    { "kvspaceResolveRef", (void *)(kvspaceResolveRef) },
    { "kvspaceRmindexExt", (void *)(kvspaceRmindexExt) },
    { "kvspaceSetPart", (void *)(kvspaceSetPart) },
    { "kvspaceSetPartByRef", (void *)(kvspaceSetPartByRef) },
    { "kvspaceSetValue", (void *)(kvspaceSetValue) },
    { "kvspaceSetValueByRef", (void *)(kvspaceSetValueByRef) },
    { "kvspaceSlotView", (void *)(kvspaceSlotView) },
    { "kvspaceViewAck", (void *)(kvspaceViewAck) },
    { "kvspaceViewCheck", (void *)(kvspaceViewCheck) },
    { "kvspaceViewDel", (void *)(kvspaceViewDel) },
    { "kvspaceViewFromRef", (void *)(kvspaceViewFromRef) },
    { "kvspaceViewQuiesce", (void *)(kvspaceViewQuiesce) },
    { "kvspaceViewRefresh", (void *)(kvspaceViewRefresh) },
    { "kvspaceViewResolve", (void *)(kvspaceViewResolve) },
    { "kvspaceViewResolveFrom", (void *)(kvspaceViewResolveFrom) },
    { "kvspaceViewResolvePrefix", (void *)(kvspaceViewResolvePrefix) },
    { "kvspaceViewSweep", (void *)(kvspaceViewSweep) },
    { "kvspaceViewWrite", (void *)(kvspaceViewWrite) },
    { "kvspaceWatch", (void *)(kvspaceWatch) },
    { "kvspaceWriteInPlace", (void *)(kvspaceWriteInPlace) },
    { "kvspaceWriteNewPlace", (void *)(kvspaceWriteNewPlace) },
    { "kvspaceXhDecode", (void *)(kvspaceXhDecode) },
    { "kvspaceXhNewCode", (void *)(kvspaceXhNewCode) },
    { "kvspaceXhNewExt", (void *)(kvspaceXhNewExt) },
    { "kvspaceXhNewNone", (void *)(kvspaceXhNewNone) },
    { "kvspaceXhNewPtr", (void *)(kvspaceXhNewPtr) },
    { "kvspaceXhNewScalar", (void *)(kvspaceXhNewScalar) },
    { "kvspaceXhNewShort", (void *)(kvspaceXhNewShort) },
    { "kvspaceXhNewSlack", (void *)(kvspaceXhNewSlack) },
    { "kvspaceXhNewTensor", (void *)(kvspaceXhNewTensor) },
    { "kvspaceXhReserve", (void *)(kvspaceXhReserve) },
    { "kvspaceXvalueEncode", (void *)(kvspaceXvalueEncode) },
    { "kvspaceXvalueEncodeMode", (void *)(kvspaceXvalueEncodeMode) },
};

__attribute__((constructor)) static void kvspace_c_register_static(void) {
    kvspaceStaticRegisterBackend("shm", kvspaceConnect, g_syms,
                                 (uint32_t)(sizeof g_syms / sizeof g_syms[0]));
}
