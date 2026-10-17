/** @file
  MM communication to the Standalone MM partition that OP-TEE hosts.

  Based on edk2-nvidia MmCommunicationOptee.

  SPDX-FileCopyrightText: Copyright (c) 2022-2025, NVIDIA CORPORATION & AFFILIATES. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/OpteeNvLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Guid/RtPropertiesTable.h>
#include <IndustryStandard/ArmStdSmc.h>
#include <Protocol/MmCommunication2.h>
#include <Protocol/NonDiscoverableDevice.h>

// OP-TEE stmm_ns_comm_buf_size
#define STMM_COMM_BUF_SIZE  SIZE_16KB

#define STMM_RT_UNSUPPORTED  (EFI_RT_SUPPORTED_GET_VARIABLE |           \
                              EFI_RT_SUPPORTED_GET_NEXT_VARIABLE_NAME | \
                              EFI_RT_SUPPORTED_SET_VARIABLE |           \
                              EFI_RT_SUPPORTED_QUERY_VARIABLE_INFO)

#define STMM_RT_ALL  ((EFI_RT_SUPPORTED_QUERY_VARIABLE_INFO << 1) - 1)

STATIC OPTEE_MESSAGE_ARG    *mMessageArg;
STATIC OPTEE_SHM_PAGE_LIST  *mPageList;
STATIC OPTEE_SHM_COOKIE     *mCookie;
STATIC VOID                 *mCommBuf;
STATIC UINT32               mSession;
STATIC BOOLEAN              mAtRuntime;
STATIC EFI_HANDLE           mMmCommunicateHandle;

STATIC EFI_GUID *CONST  mGuidedEventGuid[] = {
  &gEfiEndOfDxeEventGroupGuid,
  &gEfiEventReadyToBootGuid,
  &gEfiEventExitBootServicesGuid,
};

STATIC
EFI_STATUS
OpteeMmCommunicate (
  IN OUT VOID  *CommBuf,
  IN     UINTN  CommSize
  )
{
  EFI_MM_COMMUNICATE_HEADER  *Header;

  ZeroMem (mCommBuf, STMM_COMM_BUF_SIZE);
  CopyMem (mCommBuf, CommBuf, CommSize);

  ZeroMem (mMessageArg, sizeof (OPTEE_MESSAGE_ARG));
  mMessageArg->Command                                      = OPTEE_MESSAGE_COMMAND_INVOKE_FUNCTION;
  mMessageArg->Function                                     = OPTEE_MESSAGE_FUNCTION_STMM_COMMUNICATE;
  mMessageArg->Session                                      = mSession;
  mMessageArg->Params[0].Attribute                          = OPTEE_MESSAGE_ATTRIBUTE_TYPE_MEMORY_INOUT;
  mMessageArg->Params[0].Union.Memory.Size                  = STMM_COMM_BUF_SIZE;
  mMessageArg->Params[0].Union.Memory.SharedMemoryReference = (UINT64)mCookie;
  mMessageArg->Params[1].Attribute                          = OPTEE_MESSAGE_ATTRIBUTE_TYPE_VALUE_OUTPUT;
  mMessageArg->NumParams                                    = 2;

  if ((OpteeCallWithArg ((UINTN)mMessageArg) != 0) || (mMessageArg->Return != OPTEE_SUCCESS)) {
    DEBUG ((DEBUG_ERROR, "%a: OP-TEE returned 0x%x\n", __func__, mMessageArg->Return));
    return EFI_DEVICE_ERROR;
  }

  switch (mMessageArg->Params[1].Union.Value.A) {
    case ARM_SMC_MM_RET_SUCCESS:
      Header = mCommBuf;
      CopyMem (
        CommBuf,
        mCommBuf,
        MIN (CommSize, OFFSET_OF (EFI_MM_COMMUNICATE_HEADER, Data) + Header->MessageLength)
        );
      return EFI_SUCCESS;
    case ARM_SMC_MM_RET_INVALID_PARAMS:
      return EFI_INVALID_PARAMETER;
    case ARM_SMC_MM_RET_NO_MEMORY:
      return EFI_OUT_OF_RESOURCES;
    default:
      return EFI_ACCESS_DENIED;
  }
}

STATIC
EFI_STATUS
EFIAPI
MmCommunication2Communicate (
  IN CONST EFI_MM_COMMUNICATION2_PROTOCOL  *This,
  IN OUT VOID                              *CommBufferPhysical,
  IN OUT VOID                              *CommBufferVirtual,
  IN OUT UINTN                             *CommSize OPTIONAL
  )
{
  EFI_MM_COMMUNICATE_HEADER  *Header;
  UINTN                      BufferSize;

  if ((CommBufferVirtual == NULL) || (CommBufferPhysical == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  // RPMB requests need the eMMC driver, which the OS owns now
  if (mAtRuntime) {
    return EFI_UNSUPPORTED;
  }

  Header     = CommBufferVirtual;
  BufferSize = OFFSET_OF (EFI_MM_COMMUNICATE_HEADER, Data) + Header->MessageLength;

  if (CommSize != NULL) {
    if ((*CommSize == 0) || (*CommSize > STMM_COMM_BUF_SIZE)) {
      *CommSize = STMM_COMM_BUF_SIZE;
      return EFI_BAD_BUFFER_SIZE;
    }

    if (*CommSize < BufferSize) {
      return EFI_INVALID_PARAMETER;
    }
  }

  if ((Header->MessageLength == 0) || (BufferSize > STMM_COMM_BUF_SIZE)) {
    Header->MessageLength = STMM_COMM_BUF_SIZE - OFFSET_OF (EFI_MM_COMMUNICATE_HEADER, Data);
    return EFI_BAD_BUFFER_SIZE;
  }

  return OpteeMmCommunicate (CommBufferVirtual, BufferSize);
}

STATIC EFI_MM_COMMUNICATION2_PROTOCOL  mMmCommunication2 = {
  MmCommunication2Communicate
};

STATIC
VOID
EFIAPI
MmGuidedEventNotify (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_MM_COMMUNICATE_HEADER  Header;
  UINTN                      Size;

  // Memory services are gone at ExitBootServices, so refuse OP-TEE allocation requests
  if (CompareGuid (Context, &gEfiEventExitBootServicesGuid)) {
    OpteeLibNotifyRuntime (TRUE);
  }

  CopyGuid (&Header.HeaderGuid, Context);
  Header.MessageLength = 1;
  Header.Data[0]       = 0;

  Size = sizeof (Header);
  MmCommunication2Communicate (&mMmCommunication2, &Header, &Header, &Size);

  if (CompareGuid (Context, &gEfiEventExitBootServicesGuid)) {
    OpteeCloseSession (mSession);
    OpteeUnRegisterShm ((UINT64)mCookie);
    mAtRuntime = TRUE;
  }
}

STATIC
VOID
EFIAPI
NotifySetVirtualAddressMap (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  gRT->ConvertPointer (0, (VOID **)&mMmCommunication2.Communicate);
}

STATIC
VOID
ConnectEmmc (
  VOID
  )
{
  EFI_STATUS               Status;
  EFI_HANDLE               *Handles;
  UINTN                    Count;
  UINTN                    Index;
  NON_DISCOVERABLE_DEVICE  *Device;

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEdkiiNonDiscoverableDeviceProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    return;
  }

  for (Index = 0; Index < Count; Index++) {
    Status = gBS->HandleProtocol (Handles[Index], &gEdkiiNonDiscoverableDeviceProtocolGuid, (VOID **)&Device);
    if (!EFI_ERROR (Status) && CompareGuid (Device->Type, &gEdkiiNonDiscoverableSdhciDeviceGuid)) {
      Status = gBS->ConnectController (Handles[Index], NULL, NULL, TRUE);
      DEBUG ((DEBUG_INFO, "%a: %r\n", __func__, Status));
    }
  }

  FreePool (Handles);
}

STATIC
EFI_STATUS
OpenStmmSession (
  VOID
  )
{
  EFI_STATUS              Status;
  OPTEE_OPEN_SESSION_ARG  OpenSessionArg;

  mMessageArg = AllocateAlignedPages (EFI_SIZE_TO_PAGES (sizeof (OPTEE_MESSAGE_ARG)), OPTEE_MSG_PAGE_SIZE);
  mPageList   = AllocateAlignedPages (EFI_SIZE_TO_PAGES (sizeof (OPTEE_SHM_PAGE_LIST)), OPTEE_MSG_PAGE_SIZE);
  mCookie     = AllocateAlignedPages (EFI_SIZE_TO_PAGES (sizeof (OPTEE_SHM_COOKIE)), OPTEE_MSG_PAGE_SIZE);
  mCommBuf    = AllocateAlignedPages (EFI_SIZE_TO_PAGES (STMM_COMM_BUF_SIZE), OPTEE_MSG_PAGE_SIZE);
  if ((mMessageArg == NULL) || (mPageList == NULL) || (mCookie == NULL) || (mCommBuf == NULL)) {
    return EFI_OUT_OF_RESOURCES;
  }

  OpteeSetProperties ((UINT64)mMessageArg, (UINT64)mMessageArg, sizeof (OPTEE_MESSAGE_ARG));

  mCookie->Addr = mCommBuf;
  mCookie->Size = STMM_COMM_BUF_SIZE;
  Status        = OpteeRegisterShm (mCommBuf, (UINT64)mCookie, STMM_COMM_BUF_SIZE, mPageList);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to register SHM %r\n", __func__, Status));
    return Status;
  }

  // StMM loads on the first session and reads its store from RPMB
  ZeroMem (&OpenSessionArg, sizeof (OpenSessionArg));
  CopyGuid (&OpenSessionArg.Uuid, &gEfiSmmVariableProtocolGuid);
  Status = OpteeOpenSession (&OpenSessionArg);
  if (EFI_ERROR (Status) || (OpenSessionArg.Return != OPTEE_SUCCESS)) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to open StMM session %r 0x%x\n", __func__, Status, OpenSessionArg.Return));
    OpteeUnRegisterShm ((UINT64)mCookie);
    return EFI_UNSUPPORTED;
  }

  mSession = OpenSessionArg.Session;
  return EFI_SUCCESS;
}

STATIC
VOID
InstallRtPropertiesTable (
  VOID
  )
{
  EFI_RT_PROPERTIES_TABLE  *RtProperties;

  RtProperties = AllocateRuntimePool (sizeof (EFI_RT_PROPERTIES_TABLE));
  if (RtProperties == NULL) {
    return;
  }

  RtProperties->Version                  = EFI_RT_PROPERTIES_TABLE_VERSION;
  RtProperties->Length                   = sizeof (EFI_RT_PROPERTIES_TABLE);
  RtProperties->RuntimeServicesSupported = STMM_RT_ALL & ~STMM_RT_UNSUPPORTED;

  gBS->InstallConfigurationTable (&gEfiRtPropertiesTableGuid, RtProperties);
}

EFI_STATUS
EFIAPI
OpteeMmCommunicationInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  UINT64      Capabilities;
  EFI_EVENT   Event;
  UINTN       Index;

  if (!IsOpteePresent ()) {
    DEBUG ((DEBUG_ERROR, "%a: OP-TEE is not present\n", __func__));
    return EFI_UNSUPPORTED;
  }

  if (!OpteeExchangeCapabilities (&Capabilities) || ((Capabilities & OPTEE_SMC_SEC_CAP_DYNAMIC_SHM) == 0)) {
    DEBUG ((DEBUG_ERROR, "%a: OP-TEE lacks dynamic SHM\n", __func__));
    return EFI_UNSUPPORTED;
  }

  ConnectEmmc ();

  Status = OpenStmmSession ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &mMmCommunicateHandle,
                  &gEfiMmCommunication2ProtocolGuid,
                  &mMmCommunication2,
                  &gEfiSmmVariableProtocolGuid,
                  NULL,
                  &gSmmVariableWriteGuid,
                  NULL,
                  NULL
                  );
  ASSERT_EFI_ERROR (Status);

  Status = gBS->CreateEvent (
                  EVT_SIGNAL_VIRTUAL_ADDRESS_CHANGE,
                  TPL_NOTIFY,
                  NotifySetVirtualAddressMap,
                  NULL,
                  &Event
                  );
  ASSERT_EFI_ERROR (Status);

  for (Index = 0; Index < ARRAY_SIZE (mGuidedEventGuid); Index++) {
    Status = gBS->CreateEventEx (
                    EVT_NOTIFY_SIGNAL,
                    TPL_CALLBACK,
                    MmGuidedEventNotify,
                    mGuidedEventGuid[Index],
                    mGuidedEventGuid[Index],
                    &Event
                    );
    ASSERT_EFI_ERROR (Status);
  }

  InstallRtPropertiesTable ();

  DEBUG ((DEBUG_INFO, "%a: StMM session %u\n", __func__, mSession));
  return EFI_SUCCESS;
}
