#pragma once

#include <devioctl.h>

#define MAT_CAPTURE_PROTOCOL_VERSION 3U
#define MAT_CAPTURE_PROTOCOL_VERSION_IEC61937 2U
#define MAT_CAPTURE_DEVICE_PATH L"\\\\.\\DolbyDecoderMat"

#define IOCTL_MAT_CAPTURE_READ \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_MAT_CAPTURE_GET_STATS \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_MAT_CAPTURE_RESET \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_MAT_CAPTURE_GET_FORMAT_LOG \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_MAT_CAPTURE_RESET_FORMAT_LOG \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_WRITE_ACCESS)

#define MAT_CAPTURE_FORMAT_LOG_VERSION 1U

typedef enum _MAT_CAPTURE_FORMAT_EVENT_TYPE
{
    MatCaptureFormatEventIsFormatSupported = 1,
    MatCaptureFormatEventNewStream = 2,
    MatCaptureFormatEventWritePacket = 3
} MAT_CAPTURE_FORMAT_EVENT_TYPE;

typedef struct _MAT_CAPTURE_FORMAT_EVENT
{
    ULONGLONG Sequence;
    ULONGLONG InterruptTime100ns;
    ULONG ProcessId;
    ULONG ThreadId;
    ULONG Type;
    ULONG Pin;
    LONG Status;
    ULONG Capture;
    GUID MajorFormat;
    GUID DataSubFormat;
    GUID Specifier;
    ULONG FormatSize;
    USHORT FormatTag;
    USHORT Channels;
    ULONG SampleRate;
    ULONG AverageBytesPerSecond;
    USHORT BlockAlign;
    USHORT BitsPerSample;
    USHORT ExtraSize;
    USHORT ValidBitsPerSample;
    ULONG ChannelMask;
    GUID WaveSubFormat;
} MAT_CAPTURE_FORMAT_EVENT, *PMAT_CAPTURE_FORMAT_EVENT;

typedef struct _MAT_CAPTURE_FORMAT_LOG
{
    ULONG Version;
    ULONG HeaderBytes;
    ULONG RecordBytes;
    ULONG RecordCount;
    ULONGLONG FirstSequence;
    ULONGLONG TotalRecords;
    ULONGLONG DroppedRecords;
} MAT_CAPTURE_FORMAT_LOG, *PMAT_CAPTURE_FORMAT_LOG;

typedef struct _MAT_CAPTURE_READ_HEADER
{
    ULONG Version;
    ULONG HeaderBytes;
    ULONG PayloadBytes;
    ULONG Reserved;
    ULONGLONG FirstByteSequence;
    ULONGLONG TotalBytesWritten;
    ULONGLONG DroppedBytes;
    GUID SubFormat;
    ULONGLONG FormatChanges;
    ULONG SampleRate;
    ULONG ChannelMask;
    USHORT Channels;
    USHORT BitsPerSample;
    USHORT ValidBitsPerSample;
    USHORT BlockAlign;
} MAT_CAPTURE_READ_HEADER, *PMAT_CAPTURE_READ_HEADER;

typedef struct _MAT_CAPTURE_STATS
{
    ULONG Version;
    ULONG CapacityBytes;
    ULONG AvailableBytes;
    ULONG Reserved;
    ULONGLONG TotalBytesWritten;
    ULONGLONG TotalBytesRead;
    ULONGLONG DroppedBytes;
    GUID SubFormat;
    ULONGLONG FormatChanges;
    ULONG SampleRate;
    ULONG ChannelMask;
    USHORT Channels;
    USHORT BitsPerSample;
    USHORT ValidBitsPerSample;
    USHORT BlockAlign;
} MAT_CAPTURE_STATS, *PMAT_CAPTURE_STATS;

#if defined(_KERNEL_MODE)

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS MatCaptureInitialize(_In_ PDRIVER_OBJECT DriverObject);

_IRQL_requires_max_(PASSIVE_LEVEL)
VOID MatCaptureShutdown(_In_ PDRIVER_OBJECT DriverObject);

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID MatCaptureWrite(
    _In_ const WAVEFORMATEXTENSIBLE* Format,
    _In_reads_bytes_(ByteCount) const BYTE* Data,
    _In_ ULONG ByteCount);

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID MatCaptureRecordFormatEvent(
    _In_ MAT_CAPTURE_FORMAT_EVENT_TYPE Type,
    _In_ ULONG Pin,
    _In_ BOOLEAN Capture,
    _In_ NTSTATUS Status,
    _In_opt_ const KSDATAFORMAT* Format);

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID MatCaptureRecordPacketEvent(
    _In_ const VOID* Stream,
    _In_ const WAVEFORMATEXTENSIBLE* Format,
    _In_ ULONG PacketNumber,
    _In_ ULONG LastPacketNumber,
    _In_ KSSTATE State,
    _In_ NTSTATUS Status,
    _In_ ULONG DmaBufferSize,
    _In_ ULONG NotificationCount,
    _In_ ULONG NotificationIntervalMs);

#endif
