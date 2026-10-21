/* SPDX-License-Identifier: MIT */
/*
 * Driver entry, PnP/power, interrupt and file object handling.
 *
 * The device is the virtio-gpu PCI function of QEMU's virtio-gpu-gl. This driver does not
 * drive any display; it only gives user mode access to the 3D (virgl) command path. One
 * control queue is used; config change events (display hotplug) are ignored.
 */
#include "driver.h"

static const GUID GUID_DEVINTERFACE_VGPU = VGPU_INTERFACE_GUID_INIT;

/* required by the virtio library; its own debug output stays off */
int virtioDebugLevel = 0;
int bDebugPrint = 0;
static void NoDebugPrint(const char *Format, ...) { UNREFERENCED_PARAMETER(Format); }
tDebugPrintFunc VirtioDebugPrintProc = NoDebugPrint;

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DEVICE_PREPARE_HARDWARE EvtPrepareHardware;
static EVT_WDF_DEVICE_RELEASE_HARDWARE EvtReleaseHardware;
static EVT_WDF_DEVICE_D0_ENTRY EvtD0Entry;
static EVT_WDF_DEVICE_D0_ENTRY_POST_INTERRUPTS_ENABLED EvtD0EntryPostInterruptsEnabled;
static EVT_WDF_DEVICE_D0_EXIT EvtD0Exit;
static EVT_WDF_INTERRUPT_ISR EvtIsr;
static EVT_WDF_INTERRUPT_DPC EvtDpc;
static EVT_WDF_DEVICE_FILE_CREATE EvtFileCreate;
static EVT_WDF_FILE_CLEANUP EvtFileCleanup;
static EVT_WDF_IO_QUEUE_IO_DEFAULT EvtIoDefault;

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;

    WDF_DRIVER_CONFIG_INIT(&config, VgpuEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}

NTSTATUS VgpuEvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_FILEOBJECT_CONFIG fileConfig;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_INTERRUPT_CONFIG interruptConfig;
    WDF_IO_QUEUE_CONFIG queueConfig;
    PVGPU_DEVICE dev;
    WDFDEVICE device;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = EvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware = EvtReleaseHardware;
    pnp.EvtDeviceD0Entry = EvtD0Entry;
    pnp.EvtDeviceD0EntryPostInterruptsEnabled = EvtD0EntryPostInterruptsEnabled;
    pnp.EvtDeviceD0Exit = EvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);

    WDF_FILEOBJECT_CONFIG_INIT(&fileConfig, EvtFileCreate, WDF_NO_EVENT_CALLBACK, EvtFileCleanup);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, VGPU_FILE);
    WdfDeviceInitSetFileObjectConfig(DeviceInit, &fileConfig, &attributes);

    WdfDeviceInitSetIoInCallerContextCallback(DeviceInit, VgpuEvtIoInCallerContext);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, VGPU_DEVICE);
    status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    if (!NT_SUCCESS(status))
        return status;

    dev = VgpuGetDevice(device);
    dev->Device = device;
    KeInitializeSpinLock(&dev->QueueLock);
    InitializeListHead(&dev->FenceWaiters);
    KeInitializeEvent(&dev->RingSpace, NotificationEvent, FALSE);
    ExInitializeFastMutex(&dev->CapsetLock);
    ExInitializeFastMutex(&dev->HostVisibleLock);

    WDF_INTERRUPT_CONFIG_INIT(&interruptConfig, EvtIsr, EvtDpc);
    status = WdfInterruptCreate(device, &interruptConfig, WDF_NO_OBJECT_ATTRIBUTES, &dev->Interrupt);
    if (!NT_SUCCESS(status))
        return status;

    /* everything is completed in VgpuEvtIoInCallerContext; the queue only catches leftovers */
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoDefault = EvtIoDefault;
    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status))
        return status;

    return WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_VGPU, NULL);
}

/*
 * Find the host visible shared memory region (QEMU hostmem=): a virtio vendor capability of
 * type SHARED_MEMORY_CFG with id HOST_VISIBLE names a BAR and a range inside it. The BAR's bus
 * address is matched against the raw resources to get the translated (CPU) address.
 */
static void FindHostVisibleRegion(PVGPU_DEVICE Dev, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    PCI_COMMON_HEADER hdr;
    PPCI_COMMON_HEADER header = &hdr;
    BUS_INTERFACE_STANDARD *pci = &Dev->Virtio.PCIBus;
    UINT64 offset = 0, length = 0, bus;
    ULONG i, count, pages, guard, got;
    UCHAR pos;
    int bar = -1;

    got = pci->GetBusData(pci->Context, PCI_WHICHSPACE_CONFIG, &hdr, 0, sizeof(hdr));
    if (got != sizeof(hdr) || !(hdr.Status & PCI_STATUS_CAPABILITIES_LIST)) {
        VGPU_LOG("host visible region: PCI header read %lu bytes, status 0x%x", got, hdr.Status);
        return;
    }

    pos = hdr.u.type0.CapabilitiesPtr & ~3;
    for (guard = 0; pos >= 0x40 && guard < 48; guard++) {
        VIRTIO_PCI_CAP64 cap;

        RtlZeroMemory(&cap, sizeof(cap));
        got = pci->GetBusData(pci->Context, PCI_WHICHSPACE_CONFIG, &cap, pos, sizeof(cap));
        if (got < 2) {
            VGPU_LOG("host visible region: capability read at 0x%x returned %lu bytes", pos, got);
            break;
        }
        if (cap.CapVndr == PCI_CAPABILITY_ID_VENDOR_SPECIFIC) {
            VGPU_TRACE("virtio cap at 0x%x: type %u id %u bar %u len %u", pos, cap.CfgType, cap.Id, cap.Bar, cap.CapLen);
            if (cap.CfgType == VIRTIO_PCI_CAP_SHARED_MEMORY_CFG)
                VGPU_TRACE("shared memory cap at 0x%x: id %u bar %u offset 0x%x%08x length 0x%x%08x (cap %u bytes, read %lu)",
                         pos, cap.Id, cap.Bar, cap.OffsetHi, cap.Offset, cap.LengthHi, cap.Length, cap.CapLen, got);
            if (cap.CfgType == VIRTIO_PCI_CAP_SHARED_MEMORY_CFG && cap.Id == VIRTIO_GPU_SHM_ID_HOST_VISIBLE &&
                cap.Bar < PCI_TYPE0_ADDRESSES && got >= sizeof(cap)) {
                bar = cap.Bar;
                offset = ((UINT64)cap.OffsetHi << 32) | cap.Offset;
                length = ((UINT64)cap.LengthHi << 32) | cap.Length;
                break;
            }
        }
        pos = cap.CapNext & ~3;
    }
    if (bar < 0 || length == 0) {
        VGPU_LOG("host visible region: no shared memory capability (QEMU needs blob=true,hostmem=...)");
        return;
    }

    bus = header->u.type0.BaseAddresses[bar] & ~0xFull;
    if ((header->u.type0.BaseAddresses[bar] & 0x6) == 0x4 && bar + 1 < PCI_TYPE0_ADDRESSES)
        bus |= (UINT64)header->u.type0.BaseAddresses[bar + 1] << 32;

    count = WdfCmResourceListGetCount(Raw);
    for (i = 0; i < count; i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR raw = WdfCmResourceListGetDescriptor(Raw, i);
        PCM_PARTIAL_RESOURCE_DESCRIPTOR tr = WdfCmResourceListGetDescriptor(Translated, i);
        PHYSICAL_ADDRESS rawStart, trStart;
        UINT64 barLength;

        if (!raw || !tr || (raw->Type != CmResourceTypeMemory && raw->Type != CmResourceTypeMemoryLarge))
            continue;
        RtlCmDecodeMemIoResource(raw, (PULONGLONG)&rawStart.QuadPart);
        if ((UINT64)rawStart.QuadPart != bus)
            continue;
        barLength = RtlCmDecodeMemIoResource(tr, (PULONGLONG)&trStart.QuadPart);
        if (offset >= barLength || length > barLength - offset)
            break;
        length = min(length, VGPU_MAX_HOST_VISIBLE) & ~(UINT64)(PAGE_SIZE - 1);
        pages = (ULONG)(length >> PAGE_SHIFT);
        Dev->HostVisibleBits = ExAllocatePoolWithTag(NonPagedPoolNx, ((pages + 31) / 32) * sizeof(ULONG), VGPU_TAG);
        if (!Dev->HostVisibleBits)
            break;
        RtlInitializeBitMap(&Dev->HostVisibleMap, Dev->HostVisibleBits, pages);
        RtlClearAllBits(&Dev->HostVisibleMap);
        Dev->HostVisibleBase.QuadPart = trStart.QuadPart + offset;
        Dev->HostVisibleSize = length;
        VGPU_LOG("host visible region: BAR %d + 0x%I64x, %I64u MB", bar, offset, length >> 20);
        return;
    }
    VGPU_LOG("host visible region (BAR %d) not found in the assigned resources", bar);
}

static NTSTATUS EvtPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    PVGPU_DEVICE dev = VgpuGetDevice(Device);
    WDF_INTERRUPT_INFO info;
    ULONGLONG offered, wanted;
    NTSTATUS status;

    status = VirtIOWdfInitialize(&dev->Virtio, Device, Translated, dev->Interrupt, VGPU_TAG);
    if (!NT_SUCCESS(status)) {
        VGPU_LOG("VirtIOWdfInitialize failed 0x%08x", status);
        return status;
    }

    offered = VirtIOWdfGetDeviceFeatures(&dev->Virtio);
    if (!virtio_is_feature_enabled(offered, VIRTIO_GPU_F_VIRGL)) {
        VGPU_LOG("device offers no virgl (features 0x%I64x): use virtio-gpu-gl with a 3D-capable host", offered);
        VirtIOWdfShutdown(&dev->Virtio);
        return STATUS_NOT_SUPPORTED;
    }
    wanted = 1ull << VIRTIO_GPU_F_VIRGL;
    if (virtio_is_feature_enabled(offered, VIRTIO_GPU_F_CONTEXT_INIT))
        wanted |= 1ull << VIRTIO_GPU_F_CONTEXT_INIT;
    if (virtio_is_feature_enabled(offered, VIRTIO_RING_F_INDIRECT_DESC))
        wanted |= 1ull << VIRTIO_RING_F_INDIRECT_DESC;
    if (virtio_is_feature_enabled(offered, VIRTIO_GPU_F_RESOURCE_BLOB))
        wanted |= 1ull << VIRTIO_GPU_F_RESOURCE_BLOB;
    status = VirtIOWdfSetDriverFeatures(&dev->Virtio, wanted, 0);
    if (!NT_SUCCESS(status)) {
        VGPU_LOG("feature negotiation failed 0x%08x", status);
        VirtIOWdfShutdown(&dev->Virtio);
        return status;
    }
    dev->HasVirgl = TRUE;
    dev->HasContextInit = virtio_is_feature_enabled(wanted, VIRTIO_GPU_F_CONTEXT_INIT);
    dev->HasIndirect = virtio_is_feature_enabled(wanted, VIRTIO_RING_F_INDIRECT_DESC);
    dev->HasBlob = virtio_is_feature_enabled(wanted, VIRTIO_GPU_F_RESOURCE_BLOB);
    if (dev->HasBlob)
        FindHostVisibleRegion(dev, Raw, Translated);

    WDF_INTERRUPT_INFO_INIT(&info);
    WdfInterruptGetInfo(dev->Interrupt, &info);
    dev->MessageSignaled = info.MessageSignaled;

    VGPU_LOG("vgpu-accel %u.%u (built " __DATE__ " " __TIME__ "): features 0x%I64x -> 0x%I64x, %s interrupts",
             VGPU_DRIVER_VERSION >> 16, VGPU_DRIVER_VERSION & 0xffff, offered, wanted,
             dev->MessageSignaled ? "message-signaled" : "line");
    return STATUS_SUCCESS;
}

static NTSTATUS EvtReleaseHardware(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    PVGPU_DEVICE dev = VgpuGetDevice(Device);

    UNREFERENCED_PARAMETER(Translated);
    VgpuFreeCapsets(dev);
    if (dev->HostVisibleBits)
        ExFreePoolWithTag(dev->HostVisibleBits, VGPU_TAG);
    dev->HostVisibleBits = NULL;
    dev->HostVisibleSize = 0;
    VirtIOWdfShutdown(&dev->Virtio);
    return STATUS_SUCCESS;
}

static NTSTATUS EvtD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
    PVGPU_DEVICE dev = VgpuGetDevice(Device);
    VIRTIO_WDF_QUEUE_PARAM param;
    struct virtqueue *queue = NULL;
    NTSTATUS status;
    KIRQL irql;

    UNREFERENCED_PARAMETER(PreviousState);

    param.Interrupt = dev->Interrupt;
    status = VirtIOWdfInitQueues(&dev->Virtio, 1, &queue, &param);
    if (!NT_SUCCESS(status)) {
        VGPU_LOG("VirtIOWdfInitQueues failed 0x%08x", status);
        return status;
    }

    VirtIOWdfSetDriverOK(&dev->Virtio);

    KeAcquireSpinLock(&dev->QueueLock, &irql);
    dev->ControlQueue = queue;
    dev->InFlight = 0;
    dev->Generation++;              /* host contexts and resources from before are gone */
    dev->Ready = TRUE;
    KeReleaseSpinLock(&dev->QueueLock, irql);
    return STATUS_SUCCESS;
}

static NTSTATUS EvtD0EntryPostInterruptsEnabled(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
    UNREFERENCED_PARAMETER(PreviousState);
    /* needs completions, so only now that interrupts are on */
    return VgpuQueryCapsets(VgpuGetDevice(Device));
}

static NTSTATUS EvtD0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE TargetState)
{
    PVGPU_DEVICE dev = VgpuGetDevice(Device);
    KIRQL irql;

    UNREFERENCED_PARAMETER(TargetState);

    /* stop the device first, then fail everything it still owned */
    virtio_device_reset(&dev->Virtio.VIODevice);
    VgpuFailOutstanding(dev);

    KeAcquireSpinLock(&dev->QueueLock, &irql);
    dev->ControlQueue = NULL;
    KeReleaseSpinLock(&dev->QueueLock, irql);
    VirtIOWdfDestroyQueues(&dev->Virtio);
    return STATUS_SUCCESS;
}

static BOOLEAN EvtIsr(WDFINTERRUPT Interrupt, ULONG MessageId)
{
    PVGPU_DEVICE dev = VgpuGetDevice(WdfInterruptGetDevice(Interrupt));

    UNREFERENCED_PARAMETER(MessageId);
    /* a line interrupt may be shared: reading the ISR status also acknowledges it */
    if (!dev->MessageSignaled && VirtIOWdfGetISRStatus(&dev->Virtio) == 0)
        return FALSE;
    WdfInterruptQueueDpcForIsr(Interrupt);
    return TRUE;
}

static VOID EvtDpc(WDFINTERRUPT Interrupt, WDFOBJECT AssociatedObject)
{
    UNREFERENCED_PARAMETER(AssociatedObject);
    VgpuProcessCompletions(VgpuGetDevice(WdfInterruptGetDevice(Interrupt)));
}

static VOID EvtFileCreate(WDFDEVICE Device, WDFREQUEST Request, WDFFILEOBJECT FileObject)
{
    PVGPU_FILE file = VgpuGetFile(FileObject);

    file->Dev = VgpuGetDevice(Device);
    KeInitializeGuardedMutex(&file->Lock);
    file->State = ContextNone;
    VgpuInitResourceTable(file);
    WdfRequestComplete(Request, STATUS_SUCCESS);
}

/* last handle closed; runs in the closing process, so user mappings can be removed */
static VOID EvtFileCleanup(WDFFILEOBJECT FileObject)
{
    VgpuDestroyContext(VgpuGetFile(FileObject));
}

static VOID EvtIoDefault(WDFQUEUE Queue, WDFREQUEST Request)
{
    UNREFERENCED_PARAMETER(Queue);
    WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
}
