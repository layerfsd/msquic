/*++

    Copyright (c) Microsoft Corporation.
    Licensed under the MIT License.

Abstract:

    QUIC Perf Throughput Common definitions.

--*/


#pragma once

#define THROUGHPUT_DEFAULT_PORT 4433
#define THROUGHPUT_ALPN "Throughput"
#define THROUGHPUT_DEFAULT_DISCONNECT_TIMEOUT (10 * 1000)
#define THROUGHPUT_DEFAULT_IDLE_TIMEOUT 1000
#define THROUGHPUT_SERVER_PEER_UNI 1
#define THROUGHPUT_CLIENT_UNI 1
#define THROUGHPUT_DEFAULT_IO_SIZE_BUFFERED 0x10000
#define THROUGHPUT_DEFAULT_IO_SIZE_NONBUFFERED 0x100000
#define THROGHTPUT_DEFAULT_SEND_COUNT_BUFFERED 1
#define THROUGHPUT_DEFAULT_SEND_COUNT_NONBUFFERED 8
