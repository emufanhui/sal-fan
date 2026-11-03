#include <sysvad.h>
#include <wdmsec.h>
#include "matcapture.h"

#define MAT_CAPTURE_POOLTAG 'rCtM'
#define MAT_CAPTURE_CAPACITY (4U * 1024U * 1024U)
#define MAT_CAPTURE_FORMAT_EVENT_CAPACITY 512U

namespace
{
    const GUID MatCaptureDeviceClassGuid =
        {0x31ca452a, 0x6e3f, 0x4a6a, {0xb2, 0xe6, 0x79, 0x96, 0xf9, 0xa8, 0x3f, 0x92}};

    PDEVICE_OBJECT g_ControlDevice = nullptr;
    PDRIVER_DISPATCH g_OriginalCreate = nullptr;
    PDRIVER_DISPATCH g_OriginalClose = nullptr;
    PDRIVER_DISPATCH g_OriginalDeviceControl = nullptr;
    BYTE* g_Buffer = nullptr;
    KSPIN_LOCK g_Lock;
    ULONGLONG g_WriteSequence = 0;
    ULONGLONG g_ReadSequence = 0;
    ULONGLONG g_BytesReadByClient = 0;
    ULONGLONG g_DroppedBytes = 0;
    GUID g_SubFormat = GUID_NULL;
    ULONGLONG g_FormatChanges = 0;
    ULONG g_SampleRate = 0;
    ULONG g_ChannelMask = 0;
    USHORT g_Channels = 0;
    USHORT g_BitsPerSample = 0;
    USHORT g_ValidBitsPerSample = 0;
    USHORT g_BlockAlign = 0;
    MAT_CAPTURE_FORMAT_EVENT g_FormatEvents[MAT_CAPTURE_FORMAT_EVENT_CAPACITY] = {};
    ULONGLONG g_TotalFormatRecords = 0;

    void AppendFormatEvent(_Inout_ MAT_CAPTURE_FORMAT_EVENT* Event)
    {
        KIRQL oldIrql;
        KeAcquireSpinLock(&g_Lock, &oldIrql);
        Event->Sequence = ++g_TotalFormatRecords;
        g_FormatEvents[static_cast<ULONG>(
            (Event->Sequence - 1) % MAT_CAPTURE_FORMAT_EVENT_CAPACITY)] = *Event;
        KeReleaseSpinLock(&g_Lock, oldIrql);
    }

    bool IsCapturedFormat(_In_ const WAVEFORMATEXTENSIBLE* Format)
    {
        const GUID& SubFormat = Format->SubFormat;
        if (IsEqualGUIDAligned(SubFormat, KSDATAFORMAT_SUBTYPE_PCM))
        {
            return Format->Format.nChannels == 12 &&
                Format->Format.nSamplesPerSec == 48000 &&
                Format->dwChannelMask == 0x0002D63F &&
                Format->Format.nBlockAlign != 0 &&
                (Format->Format.wBitsPerSample == 16 ||
                 Format->Format.wBitsPerSample == 32);
        }
        return IsEqualGUIDAligned(SubFormat, KSDATAFORMAT_SUBTYPE_IEC61937_DTS) ||
            IsEqualGUIDAligned(SubFormat, KSDATAFORMAT_SUBTYPE_IEC61937_DTS_HD) ||
            IsEqualGUIDAligned(SubFormat, KSDATAFORMAT_SUBTYPE_IEC61937_DTSX_E1) ||
            IsEqualGUIDAligned(SubFormat, KSDATAFORMAT_SUBTYPE_IEC61937_DTSX_E2) ||
            IsEqualGUIDAligned(SubFormat, KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_DIGITAL) ||
            IsEqualGUIDAligned(SubFormat, KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_DIGITAL_PLUS) ||
            IsEqualGUIDAligned(SubFormat, KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_DIGITAL_PLUS_ATMOS) ||
            IsEqualGUIDAligned(SubFormat, KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_MLP) ||
            IsEqualGUIDAligned(SubFormat, KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_MAT20) ||
            IsEqualGUIDAligned(SubFormat, KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_MAT21);
    }

    bool IsSameFormat(_In_ const WAVEFORMATEXTENSIBLE* Format)
    {
        return IsEqualGUIDAligned(g_SubFormat, Format->SubFormat) &&
            g_SampleRate == Format->Format.nSamplesPerSec &&
            g_ChannelMask == Format->dwChannelMask &&
            g_Channels == Format->Format.nChannels &&
            g_BitsPerSample == Format->Format.wBitsPerSample &&
            g_ValidBitsPerSample == Format->Samples.wValidBitsPerSample &&
            g_BlockAlign == Format->Format.nBlockAlign;
    }

    void SetFormat(_In_ const WAVEFORMATEXTENSIBLE* Format)
    {
        g_SubFormat = Format->SubFormat;
        g_SampleRate = Format->Format.nSamplesPerSec;
        g_ChannelMask = Format->dwChannelMask;
        g_Channels = Format->Format.nChannels;
        g_BitsPerSample = Format->Format.wBitsPerSample;
        g_ValidBitsPerSample = Format->Samples.wValidBitsPerSample;
        g_BlockAlign = Format->Format.nBlockAlign;
    }

    void ClearFormat()
    {
        g_SubFormat = GUID_NULL;
        g_SampleRate = 0;
        g_ChannelMask = 0;
        g_Channels = 0;
        g_BitsPerSample = 0;
        g_ValidBitsPerSample = 0;
        g_BlockAlign = 0;
    }

    NTSTATUS CompleteIrp(_In_ PIRP Irp, _In_ NTSTATUS Status, _In_ ULONG_PTR Information)
    {
        Irp->IoStatus.Status = Status;
        Irp->IoStatus.Information = Information;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Status;
    }

    PDRIVER_DISPATCH OriginalDispatch(_In_ UCHAR MajorFunction)
    {
        switch (MajorFunction)
        {
        case IRP_MJ_CREATE:
            return g_OriginalCreate;
        case IRP_MJ_CLOSE:
            return g_OriginalClose;
        case IRP_MJ_DEVICE_CONTROL:
            return g_OriginalDeviceControl;
        default:
            return nullptr;
        }
    }

    NTSTATUS DispatchCreateClose(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp)
    {
        const UCHAR majorFunction = IoGetCurrentIrpStackLocation(Irp)->MajorFunction;
        if (DeviceObject != g_ControlDevice)
        {
            PDRIVER_DISPATCH original = OriginalDispatch(majorFunction);
            if (original != nullptr)
            {
                return original(DeviceObject, Irp);
            }
            return CompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
        }

        return CompleteIrp(Irp, STATUS_SUCCESS, 0);
    }

    NTSTATUS ReadCaptureData(_In_ PIRP Irp, _In_ ULONG OutputBytes)
    {
        const ULONG headerBytes = sizeof(MAT_CAPTURE_READ_HEADER);
        if (OutputBytes < headerBytes || Irp->AssociatedIrp.SystemBuffer == nullptr)
        {
            return CompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, headerBytes);
        }

        auto header = static_cast<PMAT_CAPTURE_READ_HEADER>(Irp->AssociatedIrp.SystemBuffer);
        BYTE* output = reinterpret_cast<BYTE*>(header) + headerBytes;
        const ULONG outputCapacity = OutputBytes - headerBytes;
        KIRQL oldIrql;

        KeAcquireSpinLock(&g_Lock, &oldIrql);
        const ULONGLONG available64 = g_WriteSequence - g_ReadSequence;
        const ULONG available = static_cast<ULONG>(min(available64, static_cast<ULONGLONG>(MAXULONG)));
        const ULONG payloadBytes = min(available, outputCapacity);
        const ULONGLONG firstSequence = g_ReadSequence;
        const ULONG firstOffset = static_cast<ULONG>(g_ReadSequence % MAT_CAPTURE_CAPACITY);
        const ULONG firstBytes = min(payloadBytes, MAT_CAPTURE_CAPACITY - firstOffset);

        if (firstBytes != 0)
        {
            RtlCopyMemory(output, g_Buffer + firstOffset, firstBytes);
        }
        if (payloadBytes > firstBytes)
        {
            RtlCopyMemory(output + firstBytes, g_Buffer, payloadBytes - firstBytes);
        }
        g_ReadSequence += payloadBytes;
        g_BytesReadByClient += payloadBytes;

        header->Version = MAT_CAPTURE_PROTOCOL_VERSION;
        header->HeaderBytes = headerBytes;
        header->PayloadBytes = payloadBytes;
        header->Reserved = 0;
        header->FirstByteSequence = firstSequence;
        header->TotalBytesWritten = g_WriteSequence;
        header->DroppedBytes = g_DroppedBytes;
        header->SubFormat = g_SubFormat;
        header->FormatChanges = g_FormatChanges;
        header->SampleRate = g_SampleRate;
        header->ChannelMask = g_ChannelMask;
        header->Channels = g_Channels;
        header->BitsPerSample = g_BitsPerSample;
        header->ValidBitsPerSample = g_ValidBitsPerSample;
        header->BlockAlign = g_BlockAlign;
        KeReleaseSpinLock(&g_Lock, oldIrql);

        return CompleteIrp(Irp, STATUS_SUCCESS, headerBytes + payloadBytes);
    }

    NTSTATUS GetCaptureStats(_In_ PIRP Irp, _In_ ULONG OutputBytes)
    {
        if (OutputBytes < sizeof(MAT_CAPTURE_STATS) || Irp->AssociatedIrp.SystemBuffer == nullptr)
        {
            return CompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, sizeof(MAT_CAPTURE_STATS));
        }

        auto stats = static_cast<PMAT_CAPTURE_STATS>(Irp->AssociatedIrp.SystemBuffer);
        KIRQL oldIrql;
        KeAcquireSpinLock(&g_Lock, &oldIrql);
        stats->Version = MAT_CAPTURE_PROTOCOL_VERSION;
        stats->CapacityBytes = MAT_CAPTURE_CAPACITY;
        stats->AvailableBytes = static_cast<ULONG>(g_WriteSequence - g_ReadSequence);
        stats->Reserved = 0;
        stats->TotalBytesWritten = g_WriteSequence;
        stats->TotalBytesRead = g_BytesReadByClient;
        stats->DroppedBytes = g_DroppedBytes;
        stats->SubFormat = g_SubFormat;
        stats->FormatChanges = g_FormatChanges;
        stats->SampleRate = g_SampleRate;
        stats->ChannelMask = g_ChannelMask;
        stats->Channels = g_Channels;
        stats->BitsPerSample = g_BitsPerSample;
        stats->ValidBitsPerSample = g_ValidBitsPerSample;
        stats->BlockAlign = g_BlockAlign;
        KeReleaseSpinLock(&g_Lock, oldIrql);

        return CompleteIrp(Irp, STATUS_SUCCESS, sizeof(MAT_CAPTURE_STATS));
    }

    NTSTATUS ResetCapture(_In_ PIRP Irp)
    {
        KIRQL oldIrql;
        KeAcquireSpinLock(&g_Lock, &oldIrql);
        g_WriteSequence = 0;
        g_ReadSequence = 0;
        g_BytesReadByClient = 0;
        g_DroppedBytes = 0;
        ClearFormat();
        g_FormatChanges = 0;
        KeReleaseSpinLock(&g_Lock, oldIrql);
        return CompleteIrp(Irp, STATUS_SUCCESS, 0);
    }

    NTSTATUS GetFormatLog(_In_ PIRP Irp, _In_ ULONG OutputBytes)
    {
        const ULONG headerBytes = sizeof(MAT_CAPTURE_FORMAT_LOG);
        if (OutputBytes < headerBytes || Irp->AssociatedIrp.SystemBuffer == nullptr)
        {
            return CompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, headerBytes);
        }

        auto log = static_cast<PMAT_CAPTURE_FORMAT_LOG>(Irp->AssociatedIrp.SystemBuffer);
        auto records = reinterpret_cast<PMAT_CAPTURE_FORMAT_EVENT>(
            reinterpret_cast<BYTE*>(log) + headerBytes);
        const ULONG outputRecordCapacity =
            (OutputBytes - headerBytes) / sizeof(MAT_CAPTURE_FORMAT_EVENT);

        KIRQL oldIrql;
        KeAcquireSpinLock(&g_Lock, &oldIrql);
        const ULONGLONG retained64 = min(
            g_TotalFormatRecords,
            static_cast<ULONGLONG>(MAT_CAPTURE_FORMAT_EVENT_CAPACITY));
        const ULONG retained = static_cast<ULONG>(retained64);
        const ULONG recordCount = min(retained, outputRecordCapacity);
        const ULONGLONG firstSequence =
            recordCount == 0 ? 0 : g_TotalFormatRecords - recordCount + 1;

        log->Version = MAT_CAPTURE_FORMAT_LOG_VERSION;
        log->HeaderBytes = headerBytes;
        log->RecordBytes = sizeof(MAT_CAPTURE_FORMAT_EVENT);
        log->RecordCount = recordCount;
        log->FirstSequence = firstSequence;
        log->TotalRecords = g_TotalFormatRecords;
        log->DroppedRecords = g_TotalFormatRecords > MAT_CAPTURE_FORMAT_EVENT_CAPACITY
            ? g_TotalFormatRecords - MAT_CAPTURE_FORMAT_EVENT_CAPACITY
            : 0;

        for (ULONG index = 0; index < recordCount; ++index)
        {
            const ULONGLONG sequence = firstSequence + index;
            records[index] = g_FormatEvents[
                static_cast<ULONG>((sequence - 1) % MAT_CAPTURE_FORMAT_EVENT_CAPACITY)];
        }
        KeReleaseSpinLock(&g_Lock, oldIrql);

        return CompleteIrp(
            Irp,
            STATUS_SUCCESS,
            headerBytes + recordCount * sizeof(MAT_CAPTURE_FORMAT_EVENT));
    }

    NTSTATUS ResetFormatLog(_In_ PIRP Irp)
    {
        KIRQL oldIrql;
        KeAcquireSpinLock(&g_Lock, &oldIrql);
        g_TotalFormatRecords = 0;
        RtlZeroMemory(g_FormatEvents, sizeof(g_FormatEvents));
        KeReleaseSpinLock(&g_Lock, oldIrql);
        return CompleteIrp(Irp, STATUS_SUCCESS, 0);
    }

    NTSTATUS DispatchDeviceControl(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp)
    {
        if (DeviceObject != g_ControlDevice)
        {
            if (g_OriginalDeviceControl != nullptr)
            {
                return g_OriginalDeviceControl(DeviceObject, Irp);
            }
            return CompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
        }

        const PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
        switch (stack->Parameters.DeviceIoControl.IoControlCode)
        {
        case IOCTL_MAT_CAPTURE_READ:
            return ReadCaptureData(Irp, stack->Parameters.DeviceIoControl.OutputBufferLength);
        case IOCTL_MAT_CAPTURE_GET_STATS:
            return GetCaptureStats(Irp, stack->Parameters.DeviceIoControl.OutputBufferLength);
        case IOCTL_MAT_CAPTURE_RESET:
            return ResetCapture(Irp);
        case IOCTL_MAT_CAPTURE_GET_FORMAT_LOG:
            return GetFormatLog(Irp, stack->Parameters.DeviceIoControl.OutputBufferLength);
        case IOCTL_MAT_CAPTURE_RESET_FORMAT_LOG:
            return ResetFormatLog(Irp);
        default:
            return CompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
        }
    }
}

#pragma code_seg("INIT")
NTSTATUS MatCaptureInitialize(_In_ PDRIVER_OBJECT DriverObject)
{
    UNICODE_STRING deviceName;
    UNICODE_STRING symbolicLink;
    UNICODE_STRING securityDescriptor;
    NTSTATUS status;

    KeInitializeSpinLock(&g_Lock);
    g_Buffer = static_cast<BYTE*>(ExAllocatePool2(
        POOL_FLAG_NON_PAGED, MAT_CAPTURE_CAPACITY, MAT_CAPTURE_POOLTAG));
    if (g_Buffer == nullptr)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlInitUnicodeString(&deviceName, L"\\Device\\DolbyDecoderMat");
    RtlInitUnicodeString(&securityDescriptor, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");
    status = IoCreateDeviceSecure(
        DriverObject,
        0,
        &deviceName,
        FILE_DEVICE_UNKNOWN,
        FILE_DEVICE_SECURE_OPEN,
        TRUE,
        &securityDescriptor,
        &MatCaptureDeviceClassGuid,
        &g_ControlDevice);
    if (!NT_SUCCESS(status))
    {
        ExFreePoolWithTag(g_Buffer, MAT_CAPTURE_POOLTAG);
        g_Buffer = nullptr;
        return status;
    }

    RtlInitUnicodeString(&symbolicLink, L"\\DosDevices\\DolbyDecoderMat");
    status = IoCreateSymbolicLink(&symbolicLink, &deviceName);
    if (!NT_SUCCESS(status))
    {
        IoDeleteDevice(g_ControlDevice);
        g_ControlDevice = nullptr;
        ExFreePoolWithTag(g_Buffer, MAT_CAPTURE_POOLTAG);
        g_Buffer = nullptr;
        return status;
    }

    g_OriginalCreate = DriverObject->MajorFunction[IRP_MJ_CREATE];
    g_OriginalClose = DriverObject->MajorFunction[IRP_MJ_CLOSE];
    g_OriginalDeviceControl = DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL];
    DriverObject->MajorFunction[IRP_MJ_CREATE] = DispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = DispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DispatchDeviceControl;
    g_ControlDevice->Flags |= DO_BUFFERED_IO;
    g_ControlDevice->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
VOID MatCaptureShutdown(_In_ PDRIVER_OBJECT DriverObject)
{
    PAGED_CODE();

    if (g_OriginalCreate != nullptr)
    {
        DriverObject->MajorFunction[IRP_MJ_CREATE] = g_OriginalCreate;
        DriverObject->MajorFunction[IRP_MJ_CLOSE] = g_OriginalClose;
        DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = g_OriginalDeviceControl;
    }

    UNICODE_STRING symbolicLink;
    RtlInitUnicodeString(&symbolicLink, L"\\DosDevices\\DolbyDecoderMat");
    IoDeleteSymbolicLink(&symbolicLink);

    if (g_ControlDevice != nullptr)
    {
        IoDeleteDevice(g_ControlDevice);
        g_ControlDevice = nullptr;
    }
    if (g_Buffer != nullptr)
    {
        ExFreePoolWithTag(g_Buffer, MAT_CAPTURE_POOLTAG);
        g_Buffer = nullptr;
    }
}

#pragma code_seg()
VOID MatCaptureWrite(
    _In_ const WAVEFORMATEXTENSIBLE* Format,
    _In_reads_bytes_(ByteCount) const BYTE* Data,
    _In_ ULONG ByteCount)
{
    if (g_Buffer == nullptr || Format == nullptr || Data == nullptr || ByteCount == 0 ||
        !IsCapturedFormat(Format))
    {
        return;
    }

    ULONG skippedBytes = 0;
    if (ByteCount > MAT_CAPTURE_CAPACITY)
    {
        skippedBytes = ByteCount - MAT_CAPTURE_CAPACITY;
        Data += skippedBytes;
        ByteCount = MAT_CAPTURE_CAPACITY;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&g_Lock, &oldIrql);
    if (!IsSameFormat(Format))
    {
        if (!IsEqualGUIDAligned(g_SubFormat, GUID_NULL))
        {
            ++g_FormatChanges;
            g_WriteSequence = 0;
            g_ReadSequence = 0;
            g_BytesReadByClient = 0;
            g_DroppedBytes = 0;
        }
        SetFormat(Format);
    }
    if (skippedBytes != 0)
    {
        g_WriteSequence += skippedBytes;
        if (g_WriteSequence - g_ReadSequence > MAT_CAPTURE_CAPACITY)
        {
            const ULONGLONG newReadSequence = g_WriteSequence - MAT_CAPTURE_CAPACITY;
            g_DroppedBytes += newReadSequence - g_ReadSequence;
            g_ReadSequence = newReadSequence;
        }
    }
    const ULONGLONG newWriteSequence = g_WriteSequence + ByteCount;
    if (newWriteSequence - g_ReadSequence > MAT_CAPTURE_CAPACITY)
    {
        const ULONGLONG newReadSequence = newWriteSequence - MAT_CAPTURE_CAPACITY;
        g_DroppedBytes += newReadSequence - g_ReadSequence;
        g_ReadSequence = newReadSequence;
    }

    const ULONG firstOffset = static_cast<ULONG>(g_WriteSequence % MAT_CAPTURE_CAPACITY);
    const ULONG firstBytes = min(ByteCount, MAT_CAPTURE_CAPACITY - firstOffset);
    RtlCopyMemory(g_Buffer + firstOffset, Data, firstBytes);
    if (ByteCount > firstBytes)
    {
        RtlCopyMemory(g_Buffer, Data + firstBytes, ByteCount - firstBytes);
    }
    g_WriteSequence = newWriteSequence;
    KeReleaseSpinLock(&g_Lock, oldIrql);
}

VOID MatCaptureRecordFormatEvent(
    _In_ MAT_CAPTURE_FORMAT_EVENT_TYPE Type,
    _In_ ULONG Pin,
    _In_ BOOLEAN Capture,
    _In_ NTSTATUS Status,
    _In_opt_ const KSDATAFORMAT* Format)
{
    MAT_CAPTURE_FORMAT_EVENT event = {};
    event.InterruptTime100ns = KeQueryInterruptTime();
    event.Type = Type;
    event.Pin = Pin;
    event.Status = Status;
    event.Capture = Capture ? 1U : 0U;

    if (Format != nullptr)
    {
        event.MajorFormat = Format->MajorFormat;
        event.DataSubFormat = Format->SubFormat;
        event.Specifier = Format->Specifier;
        event.FormatSize = Format->FormatSize;

        const ULONG waveOffset = sizeof(KSDATAFORMAT);
        if (Format->FormatSize >= waveOffset + sizeof(WAVEFORMATEX))
        {
            const auto wave = reinterpret_cast<const WAVEFORMATEX*>(Format + 1);
            event.FormatTag = wave->wFormatTag;
            event.Channels = wave->nChannels;
            event.SampleRate = wave->nSamplesPerSec;
            event.AverageBytesPerSecond = wave->nAvgBytesPerSec;
            event.BlockAlign = wave->nBlockAlign;
            event.BitsPerSample = wave->wBitsPerSample;
            event.ExtraSize = wave->cbSize;

            if (wave->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                Format->FormatSize >= waveOffset + sizeof(WAVEFORMATEXTENSIBLE))
            {
                const auto extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wave);
                event.ValidBitsPerSample = extensible->Samples.wValidBitsPerSample;
                event.ChannelMask = extensible->dwChannelMask;
                event.WaveSubFormat = extensible->SubFormat;
            }
        }
    }

    AppendFormatEvent(&event);
}

VOID MatCaptureRecordPacketEvent(
    _In_ const VOID* Stream,
    _In_ const WAVEFORMATEXTENSIBLE* Format,
    _In_ ULONG PacketNumber,
    _In_ ULONG LastPacketNumber,
    _In_ KSSTATE State,
    _In_ NTSTATUS Status,
    _In_ ULONG DmaBufferSize,
    _In_ ULONG NotificationCount,
    _In_ ULONG NotificationIntervalMs)
{
    if (Format == nullptr || !IsCapturedFormat(Format))
    {
        return;
    }

    MAT_CAPTURE_FORMAT_EVENT event = {};
    event.InterruptTime100ns = KeQueryInterruptTime();
    event.ProcessId = PtrToUlong(Stream);
    event.ThreadId = PtrToUlong(KeGetCurrentThread());
    event.Type = MatCaptureFormatEventWritePacket;
    event.Pin = PacketNumber;
    event.Status = Status;
    event.Capture = static_cast<ULONG>(State);
    event.FormatSize = DmaBufferSize;
    event.FormatTag = static_cast<USHORT>(LastPacketNumber & 0xFFFF);
    event.Channels = static_cast<USHORT>((LastPacketNumber >> 16) & 0xFFFF);
    event.SampleRate = NotificationIntervalMs;
    event.AverageBytesPerSecond = NotificationCount == 0
        ? 0
        : DmaBufferSize / NotificationCount;
    event.ChannelMask = NotificationCount;
    event.WaveSubFormat = Format->SubFormat;
    AppendFormatEvent(&event);
}
