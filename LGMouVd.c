/*
 * LGMouVd
 *
 * KMDF upper filter for one mouse collection. Sits between mouhid and
 * mouclass, intercepts IOCTL_INTERNAL_MOUSE_CONNECT to interpose on the
 * class service callback, and ORs MOUSE_VIRTUAL_DESKTOP into every packet
 * that carries MOUSE_MOVE_ABSOLUTE so win32k maps the 0..65535 range over
 * the whole virtual desktop instead of the primary monitor.
 *
 * Shape follows the WDK moufiltr sample.
 */

#include <ntddk.h>
#include <wdf.h>
#include <kbdmou.h>
#include <ntddmou.h>

/* CONNECT_DATA.ClassService is a PVOID; the callback round-trips through it. */
#pragma warning(disable: 4152 4055)

typedef struct _FILTER_EXTENSION
{
  CONNECT_DATA UpperConnectData;  /* mouclass's callback, saved on CONNECT */
  LONG         LoggedPackets;     /* first N packets are traced with flags */
} FILTER_EXTENSION, *PFILTER_EXTENSION;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(FILTER_EXTENSION, FilterGetData)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD LGMouVdEvtDeviceAdd;
EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL LGMouVdEvtIoInternalDeviceControl;

VOID LGMouVdServiceCallback(
  _In_    PDEVICE_OBJECT    DeviceObject,
  _In_    PMOUSE_INPUT_DATA InputDataStart,
  _In_    PMOUSE_INPUT_DATA InputDataEnd,
  _Inout_ PULONG            InputDataConsumed);

#define LGMOUVD_LOG_PACKETS 32

NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
  WDF_DRIVER_CONFIG config;
  NTSTATUS          status;

  DbgPrint("LGMouVd: DriverEntry\n");

  WDF_DRIVER_CONFIG_INIT(&config, LGMouVdEvtDeviceAdd);
  status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES,
    &config, WDF_NO_HANDLE);
  if (!NT_SUCCESS(status))
    DbgPrint("LGMouVd: WdfDriverCreate failed 0x%08X\n", status);
  return status;
}

NTSTATUS LGMouVdEvtDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
{
  WDF_OBJECT_ATTRIBUTES attributes;
  WDF_IO_QUEUE_CONFIG   queueConfig;
  WDFDEVICE             device;
  NTSTATUS              status;

  UNREFERENCED_PARAMETER(Driver);

  WdfFdoInitSetFilter(DeviceInit);
  WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_MOUSE);

  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, FILTER_EXTENSION);
  status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
  if (!NT_SUCCESS(status))
  {
    DbgPrint("LGMouVd: WdfDeviceCreate failed 0x%08X\n", status);
    return status;
  }

  WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
  queueConfig.EvtIoInternalDeviceControl = LGMouVdEvtIoInternalDeviceControl;
  status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES,
    WDF_NO_HANDLE);
  if (!NT_SUCCESS(status))
  {
    DbgPrint("LGMouVd: WdfIoQueueCreate failed 0x%08X\n", status);
    return status;
  }

  DbgPrint("LGMouVd: attached\n");
  return status;
}

VOID LGMouVdEvtIoInternalDeviceControl(_In_ WDFQUEUE Queue, _In_ WDFREQUEST Request,
  _In_ size_t OutputBufferLength, _In_ size_t InputBufferLength,
  _In_ ULONG IoControlCode)
{
  WDFDEVICE               device = WdfIoQueueGetDevice(Queue);
  PFILTER_EXTENSION       ext    = FilterGetData(device);
  PCONNECT_DATA           connectData;
  size_t                  length;
  NTSTATUS                status = STATUS_SUCCESS;
  WDF_REQUEST_SEND_OPTIONS options;
  BOOLEAN                 forwarded;

  UNREFERENCED_PARAMETER(OutputBufferLength);
  UNREFERENCED_PARAMETER(InputBufferLength);

  switch (IoControlCode)
  {
    case IOCTL_INTERNAL_MOUSE_CONNECT:
      if (ext->UpperConnectData.ClassService != NULL)
      {
        status = STATUS_SHARING_VIOLATION;
        break;
      }

      status = WdfRequestRetrieveInputBuffer(Request, sizeof(CONNECT_DATA),
        (PVOID *)&connectData, &length);
      if (!NT_SUCCESS(status))
      {
        DbgPrint("LGMouVd: CONNECT without buffer 0x%08X\n", status);
        break;
      }

      /* Remember mouclass's callback and hand mouhid our own. */
      ext->UpperConnectData       = *connectData;
      connectData->ClassDeviceObject = WdfDeviceWdmGetDeviceObject(device);
      connectData->ClassService      = LGMouVdServiceCallback;
      DbgPrint("LGMouVd: CONNECT interposed\n");
      break;

    case IOCTL_INTERNAL_MOUSE_DISCONNECT:
      ext->UpperConnectData.ClassService      = NULL;
      ext->UpperConnectData.ClassDeviceObject = NULL;
      DbgPrint("LGMouVd: DISCONNECT\n");
      break;

    default:
      break;
  }

  if (!NT_SUCCESS(status))
  {
    WdfRequestComplete(Request, status);
    return;
  }

  WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET);
  forwarded = WdfRequestSend(Request, WdfDeviceGetIoTarget(device), &options);
  if (!forwarded)
  {
    status = WdfRequestGetStatus(Request);
    DbgPrint("LGMouVd: WdfRequestSend failed 0x%08X\n", status);
    WdfRequestComplete(Request, status);
  }
}

VOID LGMouVdServiceCallback(_In_ PDEVICE_OBJECT DeviceObject,
  _In_ PMOUSE_INPUT_DATA InputDataStart, _In_ PMOUSE_INPUT_DATA InputDataEnd,
  _Inout_ PULONG InputDataConsumed)
{
  WDFDEVICE         device = WdfWdmDeviceGetWdfDeviceHandle(DeviceObject);
  PFILTER_EXTENSION ext    = FilterGetData(device);
  PMOUSE_INPUT_DATA packet;

  for (packet = InputDataStart; packet < InputDataEnd; ++packet)
  {
    USHORT before = packet->Flags;
    if (packet->Flags & MOUSE_MOVE_ABSOLUTE)
      packet->Flags |= MOUSE_VIRTUAL_DESKTOP;

    if (ext->LoggedPackets < LGMOUVD_LOG_PACKETS)
    {
      InterlockedIncrement(&ext->LoggedPackets);
      DbgPrint("LGMouVd: packet flags 0x%04X -> 0x%04X x=%ld y=%ld buttons=0x%04X\n",
        before, packet->Flags, packet->LastX, packet->LastY, packet->ButtonFlags);
    }
  }

  if (ext->UpperConnectData.ClassService != NULL)
    (*(PSERVICE_CALLBACK_ROUTINE)ext->UpperConnectData.ClassService)(
      ext->UpperConnectData.ClassDeviceObject, InputDataStart, InputDataEnd,
      InputDataConsumed);
}
