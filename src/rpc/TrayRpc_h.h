#pragma warning(disable : 4049)

#ifndef __REQUIRED_RPCNDR_H_VERSION__
#define __REQUIRED_RPCNDR_H_VERSION__ 475
#endif

#include "rpc.h"
#include "rpcndr.h"

#ifndef __RPCNDR_H_VERSION__
#error this stub requires an updated version of <rpcndr.h>
#endif

#ifndef __TrayRpc_h__
#define __TrayRpc_h__

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
#pragma once
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifndef __TrayRpc_INTERFACE_DEFINED__
#define __TrayRpc_INTERFACE_DEFINED__

void RequestStop(void);

extern handle_t hTrayRpcBinding;

extern RPC_IF_HANDLE TrayRpc_v1_0_c_ifspec;
extern RPC_IF_HANDLE TrayRpc_v1_0_s_ifspec;
#endif

#ifdef __cplusplus
}
#endif

#endif
