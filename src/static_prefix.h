/* static_prefix.h — 静态单二进制构建专用（KVSPACE_STATIC_BUILD）。
 * 前端与后端在静态链接时导出同名 kvspace* 会重复定义；这里把后端 ABI 层
 * 统一改名 kvspaceC<Name>，公共 ABI 不变（动态构建不受影响）。 */
#ifndef KVSPACE_C_STATIC_PREFIX_H
#define KVSPACE_C_STATIC_PREFIX_H

#define kvspaceClear kvspaceCClear
#define kvspaceClose kvspaceCClose
#define kvspaceConnect kvspaceCConnect
#define kvspaceCp kvspaceCCp
#define kvspaceCpList kvspaceCCpList
#define kvspaceCpTree kvspaceCCpTree
#define kvspaceDecodeHead kvspaceCDecodeHead
#define kvspaceDel kvspaceCDel
#define kvspaceDelTree kvspaceCDelTree
#define kvspaceGet kvspaceCGet
#define kvspaceGetByRef kvspaceCGetByRef
#define kvspaceGetHead kvspaceCGetHead
#define kvspaceGetPart kvspaceCGetPart
#define kvspaceListAt kvspaceCListAt
#define kvspaceListLen kvspaceCListLen
#define kvspaceMetaDecode kvspaceCMetaDecode
#define kvspaceMetaEncode kvspaceCMetaEncode
#define kvspaceMetaKey kvspaceCMetaKey
#define kvspaceMkindex kvspaceCMkindex
#define kvspaceMkindexExt kvspaceCMkindexExt
#define kvspaceNewBool kvspaceCNewBool
#define kvspaceNewChar kvspaceCNewChar
#define kvspaceNewFloat64 kvspaceCNewFloat64
#define kvspaceNewInt64 kvspaceCNewInt64
#define kvspaceNewPtr kvspaceCNewPtr
#define kvspaceResolveRef kvspaceCResolveRef
#define kvspaceRmindexExt kvspaceCRmindexExt
#define kvspaceSetPart kvspaceCSetPart
#define kvspaceSetPartByRef kvspaceCSetPartByRef
#define kvspaceSetValue kvspaceCSetValue
#define kvspaceSetValueByRef kvspaceCSetValueByRef
#define kvspaceSlotView kvspaceCSlotView
#define kvspaceViewAck kvspaceCViewAck
#define kvspaceViewCheck kvspaceCViewCheck
#define kvspaceViewDel kvspaceCViewDel
#define kvspaceViewFromRef kvspaceCViewFromRef
#define kvspaceViewQuiesce kvspaceCViewQuiesce
#define kvspaceViewRefresh kvspaceCViewRefresh
#define kvspaceViewResolve kvspaceCViewResolve
#define kvspaceViewResolveFrom kvspaceCViewResolveFrom
#define kvspaceViewResolvePrefix kvspaceCViewResolvePrefix
#define kvspaceViewSweep kvspaceCViewSweep
#define kvspaceViewWrite kvspaceCViewWrite
#define kvspaceWatch kvspaceCWatch
#define kvspaceWriteInPlace kvspaceCWriteInPlace
#define kvspaceWriteNewPlace kvspaceCWriteNewPlace
#define kvspaceXhDecode kvspaceCXhDecode
#define kvspaceXhNewCode kvspaceCXhNewCode
#define kvspaceXhNewExt kvspaceCXhNewExt
#define kvspaceXhNewNone kvspaceCXhNewNone
#define kvspaceXhNewPtr kvspaceCXhNewPtr
#define kvspaceXhNewScalar kvspaceCXhNewScalar
#define kvspaceXhNewShort kvspaceCXhNewShort
#define kvspaceXhNewSlack kvspaceCXhNewSlack
#define kvspaceXhNewTensor kvspaceCXhNewTensor
#define kvspaceXhReserve kvspaceCXhReserve
#define kvspaceXvalueEncode kvspaceCXvalueEncode
#define kvspaceXvalueEncodeMode kvspaceCXvalueEncodeMode

#endif
