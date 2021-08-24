/*++

    THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
    ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
    THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
    PARTICULAR PURPOSE.

    Copyright (c) Microsoft Corporation. All rights reserved

--*/

#pragma once

#include <evntrace.h>
#include <TraceLoggingProvider.h>
#include <evntrace.h>

TRACELOGGING_DECLARE_PROVIDER(IvshmemNetTraceProvider);

#define TraceLoggingNetAdapter(adapter) \
    TraceLoggingPointer((adapter), "NetAdapter")

#define TraceLoggingIvshmemNetAdapter(adapter) \
    TraceLoggingNetAdapter((adapter)->NetAdapter)

#define TraceLoggingFunctionName() TraceLoggingWideString(__FUNCTIONW__, "Function")

#define TraceEntry(...) \
    TraceLoggingWrite( \
        IvshmemNetTraceProvider, \
        "FunctionEntry", \
        TraceLoggingLevel(TRACE_LEVEL_VERBOSE), \
        TraceLoggingFunctionName(), \
        __VA_ARGS__)

#define TraceEntryIvshmemNetAdapter(adapter, ...) \
    TraceLoggingWrite( \
        IvshmemNetTraceProvider, \
        "FunctionEntry", \
        TraceLoggingLevel(TRACE_LEVEL_VERBOSE), \
        TraceLoggingFunctionName(), \
        TraceLoggingIvshmemNetAdapter(adapter), \
        __VA_ARGS__)

#define TraceEntryNetAdapter(netAdapter, ...) \
    TraceLoggingWrite( \
        IvshmemNetTraceProvider, \
        "FunctionEntry", \
        TraceLoggingLevel(TRACE_LEVEL_VERBOSE), \
        TraceLoggingFunctionName(), \
        TraceLoggingNetAdapter(netAdapter), \
        __VA_ARGS__)

#define TraceExit(...) \
    TraceLoggingWrite( \
        IvshmemNetTraceProvider, \
        "FunctionExit", \
        TraceLoggingLevel(TRACE_LEVEL_VERBOSE), \
        TraceLoggingFunctionName(), \
        __VA_ARGS__)

#define TraceExitResult(Status, ...) \
    TraceLoggingWrite( \
        IvshmemNetTraceProvider, \
        "FunctionExitResult", \
        TraceLoggingLevel(TRACE_LEVEL_VERBOSE), \
        TraceLoggingFunctionName(), \
        TraceLoggingNTStatus((Status), "Status"), \
        __VA_ARGS__)

#define LOG_NTSTATUS(Status, ...) do {\
        TraceLoggingWrite( \
            IvshmemNetTraceProvider, \
            "StatusFailure", \
            TraceLoggingLevel(TRACE_LEVEL_ERROR), \
            TraceLoggingFunctionName(), \
            TraceLoggingUInt32(__LINE__, "Line"), \
            TraceLoggingNTStatus(Status, "Status"), \
            __VA_ARGS__); \
} while (0,0)

#define LOG_IF_NOT_NT_SUCCESS(Expression, ...) do {\
    NTSTATUS p_status = (Expression); \
    if (!NT_SUCCESS(p_status)) \
    { \
        LOG_NTSTATUS(p_status, \
            TraceLoggingWideString(L#Expression, "Expression"), \
            __VA_ARGS__); \
    } \
} while(0,0)

#define GOTO_IF_NOT_NT_SUCCESS(Label, StatusLValue, Expression, ...) do {\
    StatusLValue = (Expression); \
    if (!NT_SUCCESS(StatusLValue)) \
    { \
        LOG_NTSTATUS(StatusLValue, \
            TraceLoggingWideString(L#Expression, "Expression"), \
            __VA_ARGS__); \
        goto Label; \
    } \
} while(0,0)

#define GOTO_WITH_INSUFFICIENT_RESOURCES_IF_NULL(Label, StatusLValue, Object) \
    GOTO_IF_NOT_NT_SUCCESS(Label, StatusLValue, (((Object) == NULL) ? STATUS_INSUFFICIENT_RESOURCES : STATUS_SUCCESS))