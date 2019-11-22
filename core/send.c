/*++

    Copyright (c) Microsoft Corporation.
    Licensed under the MIT License.

Abstract:

    Send management. The general architecture here is that anything that needs
    to be sent (data or control frames) is queued up by adding a flag to
    indicate that frame needs to be sent. There are connection-wide frames and
    stream specific frames. The Send module manages the connection-wide via the
    'SendFlags' variable. The stream specific flags are stored on the stream's
    'SendFlags' variable and the Send module maintains a list of streams that
    currently have frames that need to be sent.

    The framing and sending are done while processing the FLUSH_SEND operation.
    The operation triggers a call to QuicSendProcessFlushSendOperation which
    processes a maximum number of packets worth of data before returning out,
    so as to not starve other operations.

--*/

#include "precomp.h"

#ifdef QUIC_LOGS_WPP
#include "send.tmh"
#endif

_IRQL_requires_max_(PASSIVE_LEVEL)
void
QuicSendInitialize(
    _Inout_ PQUIC_SEND Send
    )
{
    QuicListInitializeHead(&Send->SendStreams);
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void
QuicSendUninitialize(
    _In_ PQUIC_SEND Send
    )
{
    if (Send->InitialToken != NULL) {
        QUIC_FREE(Send->InitialToken);
        Send->InitialToken = NULL;
    }

    //
    // Release all the stream refs.
    //
    QUIC_LIST_ENTRY* Entry = Send->SendStreams.Flink;
    while (Entry != &Send->SendStreams) {

        PQUIC_STREAM Stream =
            QUIC_CONTAINING_RECORD(Entry, QUIC_STREAM, SendLink);
        QUIC_DBG_ASSERT(Stream->SendFlags != 0);

        Entry = Entry->Flink;
        Stream->SendFlags = 0;
        Stream->SendLink.Flink = NULL;

        QuicStreamRelease(Stream, QUIC_STREAM_REF_SEND);
    }
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void
QuicSendApplySettings(
    _Inout_ PQUIC_SEND Send,
    _In_ const QUIC_SETTINGS* Settings
    )
{
    Send->MaxData = Settings->ConnFlowControlWindow;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void
QuicSendReset(
    _In_ PQUIC_SEND Send
    )
{
    Send->SendFlags = 0;
    Send->LastFlushTime = 0;
    if (Send->DelayedAckTimerActive) {
        LogVerbose("[send][%p] Canceling ACK_DELAY timer", QuicSendGetConnection(Send));
        QuicConnTimerCancel(QuicSendGetConnection(Send), QUIC_CONN_TIMER_ACK_DELAY);
        Send->DelayedAckTimerActive = FALSE;
    }
    QuicConnTimerCancel(
        QuicSendGetConnection(Send),
        QUIC_CONN_TIMER_PACING);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN
QuicSendCanSendFlagsNow(
    _In_ PQUIC_SEND Send
    )
{
    PQUIC_CONNECTION Connection = QuicSendGetConnection(Send);
    if (Connection->Crypto.TlsState.WriteKey < QUIC_PACKET_KEY_1_RTT &&
        Connection->Crypto.TlsState.WriteKeys[QUIC_PACKET_KEY_0_RTT] == NULL) {
        if ((!Connection->State.Started && !QuicConnIsServer(Connection)) ||
            !(Send->SendFlags & QUIC_CONN_SEND_FLAG_ALLOWED_HANDSHAKE)) {
            return FALSE;
        }
    }
    return TRUE;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
QuicSendQueueFlush(
    _In_ PQUIC_SEND Send,
    _In_ QUIC_SEND_FLUSH_REASON Reason
    )
{
    if (!Send->FlushOperationPending && QuicSendCanSendFlagsNow(Send)) {
        PQUIC_OPERATION Oper;
        PQUIC_CONNECTION Connection = QuicSendGetConnection(Send);
        if ((Oper = QuicOperationAlloc(Connection->Worker, QUIC_OPER_TYPE_FLUSH_SEND)) != NULL) {
            Send->FlushOperationPending = TRUE;
            const char* ReasonStrings[] = {
                "Flags",
                "Stream",
                "Probe",
                "Loss",
                "ACK",
                "TP",
                "CC",
                "FC",
                "NewKey",
                "StreamFC",
                "StreamID",
                "AmpProtect"
            };
            LogVerbose("[send][%p] Queuing flush (%s)", Connection, ReasonStrings[Reason]);
            QuicConnQueueOper(Connection, Oper);
        }
    }
}

#if QUIC_TEST_MODE
_IRQL_requires_max_(DISPATCH_LEVEL)
void
QuicSendValidate(
    _In_ PQUIC_SEND Send
    )
{
    PQUIC_CONNECTION Connection = QuicSendGetConnection(Send);

    BOOLEAN HasAckElicitingPacketsToAcknowledge = FALSE;
    for (uint32_t i = 0; i < QUIC_ENCRYPT_LEVEL_COUNT; ++i) {
        if (Connection->Packets[i] != NULL) {
            if (Connection->Packets[i]->AckTracker.AckElicitingPacketsToAcknowledge) {
                HasAckElicitingPacketsToAcknowledge = TRUE;
                break;
            }
        }
    }

    if (Send->SendFlags & QUIC_CONN_SEND_FLAG_ACK) {
        QUIC_DBG_ASSERT(!Send->DelayedAckTimerActive);
        QUIC_DBG_ASSERT(HasAckElicitingPacketsToAcknowledge);
    } else if (Send->DelayedAckTimerActive) {
        QUIC_DBG_ASSERT(HasAckElicitingPacketsToAcknowledge);
    } else if (!Connection->State.ClosedLocally && !Connection->State.ClosedRemotely) {
        QUIC_DBG_ASSERT(!HasAckElicitingPacketsToAcknowledge);
    }
}
#else
#define QuicSendValidate(Send)
#endif

_IRQL_requires_max_(DISPATCH_LEVEL)
void
QuicSendSetSendFlag(
    _In_ PQUIC_SEND Send,
    _In_ uint32_t SendFlags
    )
{
    PQUIC_CONNECTION Connection = QuicSendGetConnection(Send);

    BOOLEAN IsCloseFrame =
        !!(SendFlags & (QUIC_CONN_SEND_FLAG_CONNECTION_CLOSE | QUIC_CONN_SEND_FLAG_APPLICATION_CLOSE));

    BOOLEAN CanSetFlag =
        !QuicConnIsClosed(Connection) || IsCloseFrame;

    if (SendFlags & QUIC_CONN_SEND_FLAG_ACK && Send->DelayedAckTimerActive) {
        LogVerbose("[send][%p] Canceling ACK_DELAY timer", Connection);
        QuicConnTimerCancel(Connection, QUIC_CONN_TIMER_ACK_DELAY);
        Send->DelayedAckTimerActive = FALSE;
    }

    if (CanSetFlag && (Send->SendFlags & SendFlags) != SendFlags) {
        LogVerbose("[send][%p] Scheduling flags 0x%x to 0x%x",
            Connection, SendFlags, Send->SendFlags);
        Send->SendFlags |= SendFlags;
        QuicSendQueueFlush(Send, REASON_CONNECTION_FLAGS);
    }

    if (IsCloseFrame) {

        //
        // Remove all flags for things we aren't allowed to send once the connection
        // has been closed.
        //
        Send->SendFlags &= ~QUIC_CONN_SEND_FLAG_CONN_CLOSED_MASK;

        //
        // Remove any queued up streams.
        //
        while (!QuicListIsEmpty(&Send->SendStreams)) {

            PQUIC_STREAM Stream =
                QUIC_CONTAINING_RECORD(
                    QuicListRemoveHead(&Send->SendStreams), QUIC_STREAM, SendLink);

            QUIC_DBG_ASSERT(Stream->SendFlags != 0);
            Stream->SendFlags = 0;
            Stream->SendLink.Flink = NULL;

            QuicStreamRelease(Stream, QUIC_STREAM_REF_SEND);
        }
    }

    QuicSendValidate(Send);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
QuicSendClearSendFlag(
    _In_ PQUIC_SEND Send,
    _In_ uint32_t SendFlags
    )
{
    if (Send->SendFlags & SendFlags) {
        LogVerbose("[send][%p] Removing flags %x", QuicSendGetConnection(Send),
            (SendFlags & Send->SendFlags));
        Send->SendFlags &= ~SendFlags;
    }

    QuicSendValidate(Send);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
QuicSendUpdateAckState(
    _In_ PQUIC_SEND Send
    )
{
    PQUIC_CONNECTION Connection = QuicSendGetConnection(Send);

    BOOLEAN HasAckElicitingPacketsToAcknowledge = FALSE;
    for (uint32_t i = 0; i < QUIC_ENCRYPT_LEVEL_COUNT; ++i) {
        if (Connection->Packets[i] != NULL &&
            Connection->Packets[i]->AckTracker.AckElicitingPacketsToAcknowledge) {
            HasAckElicitingPacketsToAcknowledge = TRUE;
            break;
        }
    }

    if (!HasAckElicitingPacketsToAcknowledge) {
        if (Send->SendFlags & QUIC_CONN_SEND_FLAG_ACK) {
            QUIC_DBG_ASSERT(!Send->DelayedAckTimerActive);
            Send->SendFlags &= ~QUIC_CONN_SEND_FLAG_ACK;
        } else if (Send->DelayedAckTimerActive) {
            LogVerbose("[send][%p] Canceling ACK_DELAY timer", Connection);
            QuicConnTimerCancel(Connection, QUIC_CONN_TIMER_ACK_DELAY);
            Send->DelayedAckTimerActive = FALSE;
        }
    }

    QuicSendValidate(Send);
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void
QuicSendSetStreamSendFlag(
    _In_ PQUIC_SEND Send,
    _In_ PQUIC_STREAM Stream,
    _In_ uint32_t SendFlags
    )
{
    PQUIC_CONNECTION Connection = QuicSendGetConnection(Send);
    if (QuicConnIsClosed(Connection)) {
        //
        // Ignore all frames if the connection is closed.
        //
        return;
    }

    //
    // Remove any flags being queued based on the current state.
    //
    if (Stream->Flags.LocalCloseAcked) {
        SendFlags &=
            ~(QUIC_STREAM_SEND_FLAG_SEND_ABORT |
              QUIC_STREAM_SEND_FLAG_DATA_BLOCKED |
              QUIC_STREAM_SEND_FLAG_DATA |
              QUIC_STREAM_SEND_FLAG_OPEN |
              QUIC_STREAM_SEND_FLAG_FIN);
    } else if (Stream->Flags.LocalCloseReset) {
        SendFlags &=
            ~(QUIC_STREAM_SEND_FLAG_DATA_BLOCKED |
              QUIC_STREAM_SEND_FLAG_DATA |
              QUIC_STREAM_SEND_FLAG_OPEN |
              QUIC_STREAM_SEND_FLAG_FIN);
    }
    if (Stream->Flags.RemoteCloseAcked) {
        SendFlags &= ~(QUIC_STREAM_SEND_FLAG_RECV_ABORT | QUIC_STREAM_SEND_FLAG_MAX_DATA);
    } else if (Stream->Flags.RemoteCloseFin || Stream->Flags.RemoteCloseReset) {
        SendFlags &= ~QUIC_STREAM_SEND_FLAG_MAX_DATA;
    }

    if ((Stream->SendFlags | SendFlags) != Stream->SendFlags) {

        LogVerbose("[strm][%p][%llu] Setting flags 0x%x (existing flags: 0x%x)",
            Stream, Stream->ID, (SendFlags & (~Stream->SendFlags)), Stream->SendFlags);

        if ((Stream->SendFlags & SendFlags) != SendFlags) {
            //
            // Setting a new flag.
            //
            if (Stream->SendFlags == 0) {
                //
                // No flags were set previously, so add the stream to the end
                // of the queue.
                //
                QUIC_DBG_ASSERT(Stream->SendLink.Flink == NULL);
                QuicListInsertTail(&Send->SendStreams, &Stream->SendLink);
                QuicStreamAddRef(Stream, QUIC_STREAM_REF_SEND);
            }

            if (Connection->State.Started) {
                //
                // Schedule the output worker even if we didn't just queue
                // the stream, because it may have been queued and blocked.
                //
                QuicSendQueueFlush(Send, REASON_STREAM_FLAGS);
            }
        }
        Stream->SendFlags |= SendFlags;
    }
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void
QuicSendClearStreamSendFlag(
    _In_ PQUIC_SEND Send,
    _In_ PQUIC_STREAM Stream,
    _In_ uint32_t SendFlags
    )
{
    UNREFERENCED_PARAMETER(Send);

    if (Stream->SendFlags & SendFlags) {

        LogVerbose("[strm][%p][%llu] Removing flags %x",
            Stream, Stream->ID, (SendFlags & Stream->SendFlags));

        //
        // Remove the flags since they are present.
        //
        Stream->SendFlags &= ~SendFlags;

        if (Stream->SendFlags == 0) {
            //
            // Since there are no flags left, remove the stream from the queue.
            //
            QUIC_DBG_ASSERT(Stream->SendLink.Flink != NULL);
            QuicListEntryRemove(&Stream->SendLink);
            Stream->SendLink.Flink = NULL;
            QuicStreamRelease(Stream, QUIC_STREAM_REF_SEND);
        }
    }
}

_IRQL_requires_max_(PASSIVE_LEVEL)
BOOLEAN
QuicSendWriteFrames(
    _In_ PQUIC_SEND Send,
    _Inout_ QUIC_PACKET_BUILDER* Builder
    )
{
    QUIC_DBG_ASSERT(Builder->Metadata->FrameCount < QUIC_MAX_FRAMES_PER_PACKET);

    PQUIC_CONNECTION Connection = QuicSendGetConnection(Send);
    uint16_t AvailableBufferLength =
        (uint16_t)Builder->Datagram->Length - Builder->EncryptionOverhead;
    uint8_t PrevFrameCount = Builder->Metadata->FrameCount;
    BOOLEAN RanOutOfRoom = FALSE;

    QUIC_PACKET_SPACE* Packets = Connection->Packets[Builder->EncryptLevel];
    QUIC_DBG_ASSERT(Packets != NULL);

    BOOLEAN IsCongestionControlBlocked = !QuicPacketBuilderHasAllowance(Builder);

    //
    // Now fill the packet with available frames, in priority order, until we
    // run out of space. The order below was generally chosen based on the
    // perceived importance of each type of frame. ACKs are the most important
    // frame, followed by connection close and then the rest of the connection
    // specific frames.
    //

    if (Builder->PacketType != QUIC_0_RTT_PROTECTED &&
        QuicAckTrackerHasPacketsToAck(&Packets->AckTracker)) {
        if (!QuicAckTrackerAckFrameEncode(&Packets->AckTracker, Builder)) {
            RanOutOfRoom = TRUE;
            goto Exit;
        }
    }

    if (!IsCongestionControlBlocked &&
        Send->SendFlags & QUIC_CONN_SEND_FLAG_CRYPTO &&
        Builder->PacketType == QuicEncryptLevelToPacketType(QuicCryptoGetNextEncryptLevel(&Connection->Crypto))) {
        if (QuicCryptoWriteFrames(&Connection->Crypto, Builder)) {
            if (Builder->Metadata->FrameCount == QUIC_MAX_FRAMES_PER_PACKET) {
                return TRUE;
            }
        } else {
            RanOutOfRoom = TRUE;
        }
    }

    if (Send->SendFlags & (QUIC_CONN_SEND_FLAG_CONNECTION_CLOSE | QUIC_CONN_SEND_FLAG_APPLICATION_CLOSE)) {
        BOOLEAN IsApplicationClose =
            !!(Send->SendFlags & QUIC_CONN_SEND_FLAG_APPLICATION_CLOSE);
        if (Connection->State.ClosedRemotely) {
            //
            // Application closed should only be the origination of the
            // connection close. If we're closed remotely already, we should
            // just acknowledge the close with a connection close frame.
            //
            IsApplicationClose = FALSE;
        }

        QUIC_CONNECTION_CLOSE_EX Frame = {
            IsApplicationClose,
            Connection->State.ClosedRemotely ? 0 : Connection->CloseErrorCode,
            0, // TODO - Set the FrameType field.
            Connection->CloseReasonPhrase == NULL ? 0 : strlen(Connection->CloseReasonPhrase),
            Connection->CloseReasonPhrase
        };

        if (QuicConnCloseFrameEncode(
                &Frame,
                &Builder->DatagramLength,
                AvailableBufferLength,
                (uint8_t*)Builder->Datagram->Buffer)) {

            Send->SendFlags &= ~(QUIC_CONN_SEND_FLAG_CONNECTION_CLOSE | QUIC_CONN_SEND_FLAG_APPLICATION_CLOSE);
            (void)QuicPacketBuilderAddFrame(
                Builder, IsApplicationClose ? QUIC_FRAME_CONNECTION_CLOSE_1 : QUIC_FRAME_CONNECTION_CLOSE, FALSE);
        } else {
            RanOutOfRoom = TRUE;
        }

        return TRUE;
    }

    if (IsCongestionControlBlocked) {
        //
        // Everything below this is not allowed to be sent while CC blocked.
        //
        RanOutOfRoom = TRUE;
        goto Exit;
    }

    if (Send->SendFlags & QUIC_CONN_SEND_FLAG_PATH_RESPONSE) {

        uint8_t i;
        for (i = 0; i < Connection->PathsCount; ++i) {
            QUIC_PATH* TempPath = &Connection->Paths[i];
            if (!TempPath->SendResponse) {
                continue;
            }

            QUIC_PATH_RESPONSE_EX Frame = { 0 };
            QuicCopyMemory(Frame.Data, TempPath->Response, sizeof(Frame.Data));

            if (QuicPathChallengeFrameEncode(
                    QUIC_FRAME_PATH_RESPONSE,
                    &Frame,
                    &Builder->DatagramLength,
                    AvailableBufferLength,
                    (uint8_t*)Builder->Datagram->Buffer)) {

                TempPath->SendResponse = FALSE;
                Builder->Metadata->Frames[Builder->Metadata->FrameCount].PATH_RESPONSE.OrigPathId = TempPath->ID;
                QuicCopyMemory(
                    Builder->Metadata->Frames[Builder->Metadata->FrameCount].PATH_RESPONSE.Data,
                    Frame.Data,
                    sizeof(Frame.Data));
                if (QuicPacketBuilderAddFrame(Builder, QUIC_FRAME_PATH_RESPONSE, TRUE)) {
                    break;
                }
            } else {
                RanOutOfRoom = TRUE;
                break;
            }
        }

        if (i == Connection->PathsCount) {
            Send->SendFlags &= ~QUIC_CONN_SEND_FLAG_PATH_RESPONSE;
        }

        if (Builder->Metadata->FrameCount == QUIC_MAX_FRAMES_PER_PACKET) {
            return TRUE;
        }
    }

    if (Builder->Metadata->Flags.KeyType == QUIC_PACKET_KEY_1_RTT ||
        Builder->Metadata->Flags.KeyType == QUIC_PACKET_KEY_0_RTT) {

        if (Send->SendFlags & QUIC_CONN_SEND_FLAG_DATA_BLOCKED) {

            QUIC_DATA_BLOCKED_EX Frame = { Send->OrderedStreamBytesSent };

            if (QuicDataBlockedFrameEncode(
                    &Frame,
                    &Builder->DatagramLength,
                    AvailableBufferLength,
                    (uint8_t*)Builder->Datagram->Buffer)) {

                Send->SendFlags &= ~QUIC_CONN_SEND_FLAG_DATA_BLOCKED;
                if (QuicPacketBuilderAddFrame(Builder, QUIC_FRAME_DATA_BLOCKED, TRUE)) {
                    return TRUE;
                }
            } else {
                RanOutOfRoom = TRUE;
            }
        }

        if ((Send->SendFlags & QUIC_CONN_SEND_FLAG_MAX_DATA)) {

            QUIC_MAX_DATA_EX Frame = { Send->MaxData };

            if (QuicMaxDataFrameEncode(
                    &Frame,
                    &Builder->DatagramLength,
                    AvailableBufferLength,
                    (uint8_t*)Builder->Datagram->Buffer)) {

                Send->SendFlags &= ~QUIC_CONN_SEND_FLAG_MAX_DATA;
                if (QuicPacketBuilderAddFrame(Builder, QUIC_FRAME_MAX_DATA, TRUE)) {
                    return TRUE;
                }
            } else {
                RanOutOfRoom = TRUE;
            }
        }

        if ((Send->SendFlags & QUIC_CONN_SEND_FLAG_MAX_STREAMS_BIDI)) {

            QUIC_MAX_STREAMS_EX Frame = { TRUE };
            Frame.MaximumStreams =
                QuicConnIsServer(Connection) ?
                    Connection->Streams.Types[STREAM_ID_FLAG_IS_CLIENT | STREAM_ID_FLAG_IS_BI_DIR].MaxTotalStreamCount :
                    Connection->Streams.Types[STREAM_ID_FLAG_IS_SERVER | STREAM_ID_FLAG_IS_BI_DIR].MaxTotalStreamCount;

            if (QuicMaxStreamsFrameEncode(
                    &Frame,
                    &Builder->DatagramLength,
                    AvailableBufferLength,
                    (uint8_t*)Builder->Datagram->Buffer)) {

                Send->SendFlags &= ~QUIC_CONN_SEND_FLAG_MAX_STREAMS_BIDI;
                if (QuicPacketBuilderAddFrame(Builder, QUIC_FRAME_MAX_STREAMS, TRUE)) {
                    return TRUE;
                }
            } else {
                RanOutOfRoom = TRUE;
            }
        }

        if ((Send->SendFlags & QUIC_CONN_SEND_FLAG_MAX_STREAMS_UNI)) {

            QUIC_MAX_STREAMS_EX Frame = { FALSE };
            Frame.MaximumStreams =
                QuicConnIsServer(Connection) ?
                    Connection->Streams.Types[STREAM_ID_FLAG_IS_CLIENT | STREAM_ID_FLAG_IS_UNI_DIR].MaxTotalStreamCount :
                    Connection->Streams.Types[STREAM_ID_FLAG_IS_SERVER | STREAM_ID_FLAG_IS_UNI_DIR].MaxTotalStreamCount;

            if (QuicMaxStreamsFrameEncode(
                    &Frame,
                    &Builder->DatagramLength,
                    AvailableBufferLength,
                    (uint8_t*)Builder->Datagram->Buffer)) {

                Send->SendFlags &= ~QUIC_CONN_SEND_FLAG_MAX_STREAMS_UNI;
                if (QuicPacketBuilderAddFrame(Builder, QUIC_FRAME_MAX_STREAMS_1, TRUE)) {
                    return TRUE;
                }
            } else {
                RanOutOfRoom = TRUE;
            }
        }

        if ((Send->SendFlags & QUIC_CONN_SEND_FLAG_NEW_CONNECTION_ID)) {

            BOOLEAN HasMoreCidsToSend = FALSE;
            BOOLEAN MaxFrameLimitHit = FALSE;
            for (QUIC_SINGLE_LIST_ENTRY* Entry = Connection->SourceCIDs.Next;
                    Entry != NULL;
                    Entry = Entry->Next) {
                QUIC_CID_HASH_ENTRY* SourceCid =
                    QUIC_CONTAINING_RECORD(
                        Entry,
                        QUIC_CID_HASH_ENTRY,
                        Link);
                if (!SourceCid->CID.NeedsToSend) {
                    continue;
                }
                if (MaxFrameLimitHit) {
                    HasMoreCidsToSend = TRUE;
                    break;
                }

                QUIC_NEW_CONNECTION_ID_EX Frame = {
                    SourceCid->CID.Length,
                    SourceCid->CID.SequenceNumber,
                    0 };
                QuicCopyMemory(
                    Frame.Buffer,
                    SourceCid->CID.Data,
                    SourceCid->CID.Length);
                QUIC_DBG_ASSERT(SourceCid->CID.Length == MSQUIC_CONNECTION_ID_LENGTH);
                QuicBindingGenerateStatelessResetToken(
                    Builder->Path->Binding,
                    SourceCid->CID.Data,
                    Frame.Buffer + SourceCid->CID.Length);

                if (QuicNewConnectionIDFrameEncode(
                        &Frame,
                        &Builder->DatagramLength,
                        AvailableBufferLength,
                        Builder->Datagram->Buffer)) {

                    SourceCid->CID.NeedsToSend = FALSE;
                    Builder->Metadata->Frames[
                        Builder->Metadata->FrameCount].NEW_CONNECTION_ID.Sequence =
                            SourceCid->CID.SequenceNumber;
                    MaxFrameLimitHit =
                        QuicPacketBuilderAddFrame(
                            Builder, QUIC_FRAME_NEW_CONNECTION_ID, TRUE);
                } else {
                    RanOutOfRoom = TRUE;
                    HasMoreCidsToSend = TRUE;
                    break;
                }
            }
            if (!HasMoreCidsToSend) {
                Send->SendFlags &= ~QUIC_CONN_SEND_FLAG_NEW_CONNECTION_ID;
            }
        }

        if ((Send->SendFlags & QUIC_CONN_SEND_FLAG_RETIRE_CONNECTION_ID)) {

            BOOLEAN HasMoreCidsToSend = FALSE;
            BOOLEAN MaxFrameLimitHit = FALSE;
            for (QUIC_LIST_ENTRY* Entry = Connection->DestCIDs.Flink;
                    Entry != &Connection->DestCIDs;
                    Entry = Entry->Flink) {
                QUIC_CID_QUIC_LIST_ENTRY* DestCid =
                    QUIC_CONTAINING_RECORD(
                        Entry,
                        QUIC_CID_QUIC_LIST_ENTRY,
                        Link);
                if (!DestCid->CID.NeedsToSend) {
                    continue;
                }
                QUIC_DBG_ASSERT(DestCid->CID.Retired);
                if (MaxFrameLimitHit) {
                    HasMoreCidsToSend = TRUE;
                    break;
                }

                QUIC_RETIRE_CONNECTION_ID_EX Frame = {
                    DestCid->CID.SequenceNumber
                };
                if (QuicRetireConnectionIDFrameEncode(
                        &Frame,
                        &Builder->DatagramLength,
                        AvailableBufferLength,
                        Builder->Datagram->Buffer)) {

                    DestCid->CID.NeedsToSend = FALSE;
                    Builder->Metadata->Frames[
                        Builder->Metadata->FrameCount].RETIRE_CONNECTION_ID.Sequence =
                            DestCid->CID.SequenceNumber;
                    MaxFrameLimitHit =
                        QuicPacketBuilderAddFrame(
                            Builder, QUIC_FRAME_RETIRE_CONNECTION_ID, TRUE);
                } else {
                    RanOutOfRoom = TRUE;
                    HasMoreCidsToSend = TRUE;
                    break;
                }
            }
            if (!HasMoreCidsToSend) {
                Send->SendFlags &= ~QUIC_CONN_SEND_FLAG_RETIRE_CONNECTION_ID;
            }
        }
    }

    if (Send->SendFlags & QUIC_CONN_SEND_FLAG_PING) {

        if (Builder->DatagramLength < AvailableBufferLength) {
            Builder->Datagram->Buffer[Builder->DatagramLength++] = QUIC_FRAME_PING;
            Send->SendFlags &= ~QUIC_CONN_SEND_FLAG_PING;
            Builder->MinimumDatagramLength = (uint16_t)Builder->Datagram->Length;
            if (QuicPacketBuilderAddFrame(Builder, QUIC_FRAME_PING, TRUE)) {
                return TRUE;
            }
        } else {
            RanOutOfRoom = TRUE;
        }
    }

Exit:

    //
    // The only valid reason to not have framed anything is that there was too
    // little room left in the packet to fit anything more.
    //
    QUIC_DBG_ASSERT(Builder->Metadata->FrameCount > PrevFrameCount || RanOutOfRoom);

    return Builder->Metadata->FrameCount > PrevFrameCount;
}

BOOLEAN
QuicSendCanSendStreamNow(
    _In_ PQUIC_STREAM Stream
    )
{
    QUIC_DBG_ASSERT(Stream->SendFlags != 0);

    PQUIC_CONNECTION Connection = Stream->Connection;

    if (Connection->Crypto.TlsState.WriteKey == QUIC_PACKET_KEY_1_RTT) {
        return QuicStreamCanSendNow(Stream, FALSE);
    } else if (Connection->Crypto.TlsState.WriteKeys[QUIC_PACKET_KEY_0_RTT] != NULL) {
        return QuicStreamCanSendNow(Stream, TRUE);
    }

    return FALSE;
}

_Success_(return != NULL)
PQUIC_STREAM
QuicSendGetNextStream(
    _In_ PQUIC_SEND Send,
    _Out_ uint32_t* PacketCount
    )
{
    QUIC_DBG_ASSERT(!QuicConnIsClosed(QuicSendGetConnection(Send)) || QuicListIsEmpty(&Send->SendStreams));

    QUIC_LIST_ENTRY* Entry = Send->SendStreams.Flink;
    while (Entry != &Send->SendStreams) {

        //
        // TODO: performance: We currently search through blocked
        // streams repeatedly as we loop.
        //

        PQUIC_STREAM Stream = QUIC_CONTAINING_RECORD(Entry, QUIC_STREAM, SendLink);

        //
        // Make sure, given the current state of the connection and the stream,
        // that we can use the stream to frame a packet.
        //
        if (QuicSendCanSendStreamNow(Stream)) {

            //
            // Move the stream to the end of the queue.
            //
            QuicListEntryRemove(&Stream->SendLink);
            QuicListInsertTail(&Send->SendStreams, &Stream->SendLink);

            *PacketCount = QUIC_STREAM_SEND_BATCH_COUNT;
            return Stream;
        }

        Entry = Entry->Flink;
    }

    return NULL;
}

//
// This function sends a path challenge frame out on all paths that currently
// need one sent.
//
_IRQL_requires_max_(PASSIVE_LEVEL)
void
QuicSendPathChallenges(
    _In_ PQUIC_SEND Send
    )
{
    PQUIC_CONNECTION Connection = QuicSendGetConnection(Send);

    QUIC_DBG_ASSERT(Connection->Crypto.TlsState.WriteKeys[QUIC_PACKET_KEY_1_RTT] != NULL);

    for (uint8_t i = 0; i < Connection->PathsCount; ++i) {

        QUIC_PATH* Path = &Connection->Paths[i];
        if (!Connection->Paths[i].SendChallenge) {
            continue;
        }

        QUIC_PACKET_BUILDER Builder = { 0 };
        if (!QuicPacketBuilderInitialize(&Builder, Connection, Path)) {
            continue;
        }
        _Analysis_assume_(Builder.Metadata != NULL);

        if (!QuicPacketBuilderPrepareForControlFrames(
                &Builder, FALSE, QUIC_CONN_SEND_FLAG_PATH_CHALLENGE)) {
            continue;
        }

        uint16_t AvailableBufferLength =
            (uint16_t)Builder.Datagram->Length - Builder.EncryptionOverhead;

        QUIC_PATH_CHALLENGE_EX Frame;
        QuicCopyMemory(Frame.Data, Path->Challenge, sizeof(Frame.Data));

        BOOLEAN Result =
            QuicPathChallengeFrameEncode(
                QUIC_FRAME_PATH_CHALLENGE,
                &Frame,
                &Builder.DatagramLength,
                AvailableBufferLength,
                Builder.Datagram->Buffer);
        QUIC_DBG_ASSERT(Result);

        QuicCopyMemory(
            Builder.Metadata->Frames[0].PATH_CHALLENGE.Data,
            Frame.Data,
            sizeof(Frame.Data));

        Result = QuicPacketBuilderAddFrame(&Builder, QUIC_FRAME_PATH_CHALLENGE, TRUE);
        QUIC_DBG_ASSERT(!Result);

        QuicPacketBuilderFinalize(&Builder, TRUE);

        Path->SendChallenge = FALSE;
    }
}

typedef enum _QUIC_SEND_RESULT {

    QUIC_SEND_COMPLETE,
    QUIC_SEND_INCOMPLETE,
    QUIC_SEND_DELAYED_PACING

} QUIC_SEND_RESULT;

//
// Sends items from the output queue.
//
_IRQL_requires_max_(PASSIVE_LEVEL)
QUIC_SEND_RESULT
QuicSendFlush(
    _In_ PQUIC_SEND Send
    )
{
    PQUIC_CONNECTION Connection = QuicSendGetConnection(Send);

    QuicConnRemoveOutFlowBlockedReason(
        Connection, QUIC_FLOW_BLOCKED_SCHEDULING);

    if (Send->SendFlags == 0 && QuicListIsEmpty(&Send->SendStreams)) {
        return QUIC_SEND_COMPLETE;
    }

    QUIC_PATH* Path = &Connection->Paths[0]; // TODO - How to pick?
    if (Path->DestCid == NULL) {
        return QUIC_SEND_COMPLETE;
    }

    QUIC_DBG_ASSERT(QuicSendCanSendFlagsNow(Send));

    QUIC_SEND_RESULT Result = QUIC_SEND_INCOMPLETE;
    PQUIC_STREAM Stream = NULL;
    uint32_t StreamPacketCount = 0;

    if (Send->SendFlags & QUIC_CONN_SEND_FLAG_PATH_CHALLENGE) {
        Send->SendFlags &= ~QUIC_CONN_SEND_FLAG_PATH_CHALLENGE;
        QuicSendPathChallenges(Send);
    }

    QUIC_PACKET_BUILDER Builder = { 0 };
    if (!QuicPacketBuilderInitialize(&Builder, Connection, Path)) {
        //
        // If this fails, the connection is in a bad (likely partially
        // uninitialized) state, so just ignore the send flush call. This can
        // happen if a loss detection fires right after shutdown.
        //
        return QUIC_SEND_COMPLETE;
    }
    _Analysis_assume_(Builder.Metadata != NULL);

    LogVerbose("[send][%p] Flushing send. Allowance=%u bytes", Connection, Builder.SendAllowance);

    do {

        if (Path->Allowance < QUIC_MIN_SEND_ALLOWANCE) {
            LogVerbose("[conn][%p] Cannot send any more because of amplification protection", Connection);
            Result = QUIC_SEND_COMPLETE;
            break;
        }

        uint32_t SendFlags = Send->SendFlags;
        if (Connection->Crypto.TlsState.WriteKey < QUIC_PACKET_KEY_1_RTT &&
            Connection->Crypto.TlsState.WriteKeys[QUIC_PACKET_KEY_0_RTT] == NULL) {
            SendFlags &= QUIC_CONN_SEND_FLAG_ALLOWED_HANDSHAKE;
        }

        if (!QuicPacketBuilderHasAllowance(&Builder)) {
            //
            // While we are CC blocked, very few things are still allowed to
            // be sent. If those are queued then we can still send.
            //
            if (!(SendFlags & QUIC_CONN_SEND_FLAGS_BYPASS_CC)) {
                if (QuicCongestionControlCanSend(&Connection->CongestionControl)) {
                    //
                    // The current pacing chunk is finished. We need to schedule a
                    // new pacing send.
                    //
                    LogVerbose("[send][%p] Setting delayed send (PACING) timer for %u ms",
                        Connection, QUIC_SEND_PACING_INTERVAL);
                    QuicConnTimerSet(
                        Connection,
                        QUIC_CONN_TIMER_PACING,
                        QUIC_SEND_PACING_INTERVAL);
                    Result = QUIC_SEND_DELAYED_PACING;
                } else {
                    //
                    // No pure ACKs to send right now. All done sending for now.
                    //
                    Result = QUIC_SEND_COMPLETE;
                }
                break;
            }
        }

        //
        // We write data to packets in the following order:
        //
        //   1. Connection wide control data.
        //   2. Stream (control and application) data.
        //   3. Path MTU discovery packets.
        //

        BOOLEAN WrotePacketFrames;
        BOOLEAN FlushBatchedDatagrams = FALSE;
        if ((SendFlags & ~QUIC_CONN_SEND_FLAG_PMTUD) != 0) {
            if (!QuicPacketBuilderPrepareForControlFrames(
                    &Builder,
                    Send->TailLossProbeNeeded,
                    Send->SendFlags & ~QUIC_CONN_SEND_FLAG_PMTUD)) {
                break;
            }
            WrotePacketFrames = QuicSendWriteFrames(Send, &Builder);

        } else if (Stream != NULL ||
            (Stream = QuicSendGetNextStream(Send, &StreamPacketCount)) != NULL) {
            if (!QuicPacketBuilderPrepareForStreamFrames(
                    &Builder,
                    Send->TailLossProbeNeeded)) {
                break;
            }
            WrotePacketFrames = QuicStreamSendWrite(Stream, &Builder);

            if (Stream->SendFlags == 0) {
                //
                // If the stream no longer has anything to send, remove it from the
                // list and release Send's reference on it.
                //
                QuicListEntryRemove(&Stream->SendLink);
                Stream->SendLink.Flink = NULL;
                QuicStreamRelease(Stream, QUIC_STREAM_REF_SEND);
                Stream = NULL;

            } else if ((WrotePacketFrames && --StreamPacketCount == 0) ||
                !QuicSendCanSendStreamNow(Stream)) {
                //
                // Try a new stream next loop iteration.
                //
                Stream = NULL;
            }

        } else if (SendFlags == QUIC_CONN_SEND_FLAG_PMTUD) {
            if (!QuicPacketBuilderPrepareForPathMtuDiscovery(&Builder)) {
                break;
            }
            FlushBatchedDatagrams = TRUE;
            Send->SendFlags &= ~QUIC_CONN_SEND_FLAG_PMTUD;
            if (Builder.Metadata->FrameCount < QUIC_MAX_FRAMES_PER_PACKET &&
                Builder.DatagramLength < Builder.Datagram->Length - Builder.EncryptionOverhead) {
                //
                // We are doing PMTUD, so make sure there is a PING frame in there, if
                // we have room, just to make sure we get an ACK.
                //
                Builder.Datagram->Buffer[Builder.DatagramLength++] = QUIC_FRAME_PING;
                Builder.Metadata->Frames[Builder.Metadata->FrameCount++].Type = QUIC_FRAME_PING;
                WrotePacketFrames = TRUE;
            } else {
                WrotePacketFrames = FALSE;
            }

        } else {
            //
            // Nothing else left to send right now.
            //
            Result = QUIC_SEND_COMPLETE;
            break;
        }

        Send->TailLossProbeNeeded = FALSE;

        if (!WrotePacketFrames ||
            Builder.Metadata->FrameCount == QUIC_MAX_FRAMES_PER_PACKET ||
            Builder.Datagram->Length - Builder.DatagramLength < QUIC_MIN_PACKET_SPARE_SPACE) {

            //
            // We now have enough data in the current packet that we should
            // finalize it.
            //
            QuicPacketBuilderFinalize(&Builder, FlushBatchedDatagrams);
        }

    } while (Builder.SendContext != NULL ||
        Builder.TotalCountDatagrams < QUIC_MAX_DATAGRAMS_PER_SEND);

    if (Result == QUIC_SEND_INCOMPLETE &&
        Builder.TotalCountDatagrams >= QUIC_MAX_DATAGRAMS_PER_SEND) {
        //
        // The send is limited by the scheduling logic.
        //
        QuicConnAddOutFlowBlockedReason(
            Connection, QUIC_FLOW_BLOCKED_SCHEDULING);
    }

    QuicPacketBuilderCleanup(&Builder);

    LogVerbose("[send][%p] Flush complete flags=0x%x", Connection, Send->SendFlags);

    return Result;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
BOOLEAN
QuicSendProcessFlushSendOperation(
    _In_ PQUIC_SEND Send,
    _In_ BOOLEAN Immediate
    )
{
    PQUIC_CONNECTION Connection = QuicSendGetConnection(Send);

    QUIC_DBG_ASSERT(!Connection->State.HandleClosed);

    QuicConnTimerCancel(Connection, QUIC_CONN_TIMER_PACING);
    QUIC_SEND_RESULT SendResult = QuicSendFlush(Send);

    if (!Immediate && SendResult != QUIC_SEND_INCOMPLETE) {
        //
        // We have no more data to immediately send out so clear the pending
        // flag.
        //
        Send->FlushOperationPending = FALSE;
    }

    return SendResult == QUIC_SEND_INCOMPLETE;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
QuicSendStartDelayedAckTimer(
    _In_ PQUIC_SEND Send
    )
{
    PQUIC_CONNECTION Connection = QuicSendGetConnection(Send);

    if (!Send->DelayedAckTimerActive &&
        !(Send->SendFlags & QUIC_CONN_SEND_FLAG_ACK) &&
        !Connection->State.ClosedLocally &&
        !Connection->State.ClosedRemotely) {

        LogVerbose("[send][%p] Starting ACK_DELAY timer for %u ms",
            Connection, Connection->MaxAckDelayMs);
        QuicConnTimerSet(
            Connection,
            QUIC_CONN_TIMER_ACK_DELAY,
            Connection->MaxAckDelayMs); // TODO - Use smaller timeout when handshake data is outstanding.
        Send->DelayedAckTimerActive = TRUE;
    }
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
QuicSendProcessDelayedAckTimer(
    _In_ PQUIC_SEND Send
    )
{
    QUIC_DBG_ASSERT(Send->DelayedAckTimerActive);
    QUIC_DBG_ASSERT(!(Send->SendFlags & QUIC_CONN_SEND_FLAG_ACK));
    Send->DelayedAckTimerActive = FALSE;

    PQUIC_CONNECTION Connection = QuicSendGetConnection(Send);

    BOOLEAN AckElicitingPacketsToAcknowledge = FALSE;
    for (uint32_t i = 0; i < QUIC_ENCRYPT_LEVEL_COUNT; ++i) {
        if (Connection->Packets[i] != NULL &&
            Connection->Packets[i]->AckTracker.AckElicitingPacketsToAcknowledge) {
            AckElicitingPacketsToAcknowledge = TRUE;
            break;
        }
    }

    QUIC_DBG_ASSERT(AckElicitingPacketsToAcknowledge);
    if (AckElicitingPacketsToAcknowledge) {
        Send->SendFlags |= QUIC_CONN_SEND_FLAG_ACK;
    }

    QuicSendValidate(Send);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
QuicSendOnMtuProbePacketAcked(
    _In_ PQUIC_SEND Send,
    _In_ QUIC_PATH* Path,
    _In_ PQUIC_SENT_PACKET_METADATA Packet
    )
{
    Path->Mtu =
        PacketSizeFromUdpPayloadSize(
            QuicAddrGetFamily(&Path->RemoteAddress),
            Packet->PacketLength);
    LogInfo("[conn][%p] Path[%hu] MTU updated to %u bytes",
        QuicSendGetConnection(Send), Path->ID, Path->Mtu);
}