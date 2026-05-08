#if defined(_M_AMD64)

#pragma warning(disable : 4049)
#if _MSC_VER >= 1200
#pragma warning(push)
#endif

#pragma warning(disable : 4211)
#pragma warning(disable : 4232)
#pragma warning(disable : 4024)
#pragma warning(disable : 4100)

#pragma optimize("", off)

#include <string.h>

#include "TrayRpc_h.h"

#define TYPE_FORMAT_STRING_SIZE 1
#define PROC_FORMAT_STRING_SIZE 25
#define EXPR_FORMAT_STRING_SIZE 1
#define TRANSMIT_AS_TABLE_SIZE 0
#define WIRE_MARSHAL_TABLE_SIZE 0

typedef struct _TrayRpc_MIDL_TYPE_FORMAT_STRING {
  short Pad;
  unsigned char Format[TYPE_FORMAT_STRING_SIZE];
} TrayRpc_MIDL_TYPE_FORMAT_STRING;

typedef struct _TrayRpc_MIDL_PROC_FORMAT_STRING {
  short Pad;
  unsigned char Format[PROC_FORMAT_STRING_SIZE];
} TrayRpc_MIDL_PROC_FORMAT_STRING;

typedef struct _TrayRpc_MIDL_EXPR_FORMAT_STRING {
  long Pad;
  unsigned char Format[EXPR_FORMAT_STRING_SIZE];
} TrayRpc_MIDL_EXPR_FORMAT_STRING;

static const RPC_SYNTAX_IDENTIFIER _RpcTransferSyntax = {
    {0x8A885D04, 0x1CEB, 0x11C9, {0x9F, 0xE8, 0x08, 0x00, 0x2B, 0x10, 0x48, 0x60}},
    {2, 0}};

extern const TrayRpc_MIDL_TYPE_FORMAT_STRING TrayRpc__MIDL_TypeFormatString;
extern const TrayRpc_MIDL_PROC_FORMAT_STRING TrayRpc__MIDL_ProcFormatString;
extern const TrayRpc_MIDL_EXPR_FORMAT_STRING TrayRpc__MIDL_ExprFormatString;

extern const MIDL_SERVER_INFO TrayRpc_ServerInfo;
extern const RPC_DISPATCH_TABLE TrayRpc_v1_0_DispatchTable;

static const RPC_SERVER_INTERFACE TrayRpc___RpcServerInterface = {
    sizeof(RPC_SERVER_INTERFACE),
    {{0x8b4e7c3b, 0x5a1f, 0x4b2e, {0x9c, 0x11, 0x0f, 0x3e, 0x8d, 0x7a, 0x6b, 0x05}}, {1, 0}},
    {{0x8A885D04, 0x1CEB, 0x11C9, {0x9F, 0xE8, 0x08, 0x00, 0x2B, 0x10, 0x48, 0x60}}, {2, 0}},
    (RPC_DISPATCH_TABLE*)&TrayRpc_v1_0_DispatchTable,
    0,
    0,
    0,
    &TrayRpc_ServerInfo,
    0x04000000};
RPC_IF_HANDLE TrayRpc_v1_0_s_ifspec = (RPC_IF_HANDLE)&TrayRpc___RpcServerInterface;

extern const MIDL_STUB_DESC TrayRpc_StubDesc;

#if !defined(__RPC_WIN64__)
#error Invalid build platform for this stub.
#endif

#if !(TARGET_IS_NT50_OR_LATER)
#error You need Windows 2000 or later to run this stub because it uses /robust.
#endif

static const TrayRpc_MIDL_PROC_FORMAT_STRING TrayRpc__MIDL_ProcFormatString = {
    0,
    {
        0x32, 0x48, NdrFcLong(0x0), NdrFcShort(0x0), NdrFcShort(0x0), NdrFcShort(0x0),
        NdrFcShort(0x0), 0x40, 0x0, 0xa, 0x1, NdrFcShort(0x0), NdrFcShort(0x0),
        NdrFcShort(0x0), NdrFcShort(0x0), 0x0}};

static const TrayRpc_MIDL_TYPE_FORMAT_STRING TrayRpc__MIDL_TypeFormatString = {
    0,
    {0x0}};

static const unsigned short TrayRpc_FormatStringOffsetTable[] = {
    0,
};

static const MIDL_STUB_DESC TrayRpc_StubDesc = {
    (void*)&TrayRpc___RpcServerInterface,
    MIDL_user_allocate,
    MIDL_user_free,
    0,
    0,
    0,
    0,
    0,
    TrayRpc__MIDL_TypeFormatString.Format,
    1,
    0x50002,
    0,
    0x801026e,
    0,
    0,
    0,
    0x1,
    0,
    0,
    0};

static const RPC_DISPATCH_FUNCTION TrayRpc_table[] = {
    NdrServerCall2,
    0,
};
static const RPC_DISPATCH_TABLE TrayRpc_v1_0_DispatchTable = {
    1,
    (RPC_DISPATCH_FUNCTION*)TrayRpc_table};

static const SERVER_ROUTINE TrayRpc_ServerRoutineTable[] = {
    (SERVER_ROUTINE)RequestStop,
};

static const MIDL_SERVER_INFO TrayRpc_ServerInfo = {
    &TrayRpc_StubDesc,
    TrayRpc_ServerRoutineTable,
    TrayRpc__MIDL_ProcFormatString.Format,
    TrayRpc_FormatStringOffsetTable,
    0,
    0,
    0,
    0};

#if _MSC_VER >= 1200
#pragma warning(pop)
#endif

#endif /* defined(_M_AMD64) */
