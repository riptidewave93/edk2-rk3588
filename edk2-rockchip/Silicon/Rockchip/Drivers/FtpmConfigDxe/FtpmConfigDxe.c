/** @file

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>
#include <IndustryStandard/Tpm20.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/HiiLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/OpteeNvLib.h>
#include <Library/PcdLib.h>
#include <Library/PrintLib.h>
#include <Library/Tpm2CommandLib.h>
#include <Library/TpmPlatformHierarchyLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/Tcg2Protocol.h>

#include "FtpmConfigDxe.h"

#define PTA_RK_FTPM_EMBEDDED      BIT0
#define PTA_RK_FTPM_STORAGE_RPMB  BIT1

#define PTA_RK_FTPM_CMD_GET_INFO  0
#define PTA_RK_FTPM_CMD_DISABLE   1
#define PTA_RK_FTPM_CMD_LOCK      2

extern UINT8  FtpmConfigDxeHiiBin[];
extern UINT8  FtpmConfigDxeStrings[];

typedef struct {
  VENDOR_DEVICE_PATH          VendorDevicePath;
  EFI_DEVICE_PATH_PROTOCOL    End;
} HII_VENDOR_DEVICE_PATH;

STATIC HII_VENDOR_DEVICE_PATH  mVendorDevicePath = {
  {
    {
      HARDWARE_DEVICE_PATH,
      HW_VENDOR_DP,
      {
        (UINT8)(sizeof (VENDOR_DEVICE_PATH)),
        (UINT8)((sizeof (VENDOR_DEVICE_PATH)) >> 8)
      }
    },
    FTPM_CONFIG_FORMSET_GUID
  },
  {
    END_DEVICE_PATH_TYPE,
    END_ENTIRE_DEVICE_PATH_SUBTYPE,
    {
      (UINT8)(END_DEVICE_PATH_LENGTH),
      (UINT8)((END_DEVICE_PATH_LENGTH) >> 8)
    }
  }
};

STATIC EFI_HII_HANDLE             mHiiHandle;
STATIC FTPM_CONFIG_VARSTORE_DATA  mConfig;
STATIC FTPM_INFO_VARSTORE_DATA    mInfo;
STATIC OPTEE_MESSAGE_ARG          *mPtaMessage;
STATIC UINT32                     mPtaSession;
STATIC BOOLEAN                    mResetPending;

STATIC
EFI_STATUS
PtaOpen (
  VOID
  )
{
  EFI_STATUS              Status;
  OPTEE_OPEN_SESSION_ARG  Arg;

  mPtaMessage = AllocateAlignedPages (1, OPTEE_MSG_PAGE_SIZE);
  if (mPtaMessage == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  OpteeSetProperties ((UINTN)mPtaMessage, (UINTN)mPtaMessage, EFI_PAGE_SIZE);

  ZeroMem (&Arg, sizeof (Arg));
  CopyGuid (&Arg.Uuid, &gRockchipFtpmControlPtaGuid);
  Status = OpteeOpenSession (&Arg);
  if (EFI_ERROR (Status) || (Arg.Return != OPTEE_SUCCESS)) {
    FreeAlignedPages (mPtaMessage, 1);
    mPtaMessage = NULL;
    return EFI_NOT_FOUND;
  }

  mPtaSession = Arg.Session;
  return EFI_SUCCESS;
}

STATIC
VOID
PtaClose (
  VOID
  )
{
  OpteeCloseSession (mPtaSession);
  FreeAlignedPages (mPtaMessage, 1);
  mPtaMessage = NULL;
}

STATIC
EFI_STATUS
PtaInvoke (
  IN  UINT32  Command,
  OUT UINT32  *Value OPTIONAL
  )
{
  EFI_STATUS                 Status;
  OPTEE_INVOKE_FUNCTION_ARG  Arg;

  ZeroMem (&Arg, sizeof (Arg));
  Arg.Function = Command;
  Arg.Session  = mPtaSession;
  if (Value != NULL) {
    Arg.Params[0].Attribute = OPTEE_MESSAGE_ATTRIBUTE_TYPE_VALUE_OUTPUT;
  }

  Status = OpteeInvokeFunction (&Arg);
  if (EFI_ERROR (Status) || (Arg.Return != OPTEE_SUCCESS)) {
    DEBUG ((DEBUG_ERROR, "%a: Command %u failed, OP-TEE returned 0x%x\n", __func__, Command, Arg.Return));
    return EFI_DEVICE_ERROR;
  }

  if (Value != NULL) {
    *Value = (UINT32)Arg.Params[0].Union.Value.A;
  }

  return EFI_SUCCESS;
}

STATIC
VOID
SaveConfig (
  VOID
  )
{
  EFI_STATUS  Status;

  Status = gRT->SetVariable (
                  FTPM_CONFIG_DATA_VAR_NAME,
                  &gFtpmConfigFormSetGuid,
                  EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS,
                  sizeof (mConfig),
                  &mConfig
                  );
  ASSERT_EFI_ERROR (Status);
}

STATIC
VOID
SaveInfo (
  VOID
  )
{
  EFI_STATUS  Status;

  Status = gRT->SetVariable (
                  FTPM_INFO_DATA_VAR_NAME,
                  &gFtpmConfigFormSetGuid,
                  EFI_VARIABLE_BOOTSERVICE_ACCESS,
                  sizeof (mInfo),
                  &mInfo
                  );
  ASSERT_EFI_ERROR (Status);
}

STATIC
VOID
LoadConfig (
  VOID
  )
{
  EFI_STATUS  Status;
  UINTN       Size;

  Size   = sizeof (mConfig);
  Status = gRT->GetVariable (
                  FTPM_CONFIG_DATA_VAR_NAME,
                  &gFtpmConfigFormSetGuid,
                  NULL,
                  &Size,
                  &mConfig
                  );
  if (!EFI_ERROR (Status) && (Size == sizeof (mConfig))) {
    return;
  }

  ZeroMem (&mConfig, sizeof (mConfig));
  mConfig.Enabled       = TRUE;
  mConfig.Operation     = FTPM_OPERATION_NONE;
  mConfig.PcrBankSha1   = TRUE;
  mConfig.PcrBankSha256 = TRUE;
  mConfig.PcrBankSha384 = TRUE;
  SaveConfig ();
}

STATIC
UINT32
GetConfigPcrBanks (
  VOID
  )
{
  UINT32  Banks;

  Banks = 0;
  if (mConfig.PcrBankSha1) {
    Banks |= EFI_TCG2_BOOT_HASH_ALG_SHA1;
  }

  if (mConfig.PcrBankSha256) {
    Banks |= EFI_TCG2_BOOT_HASH_ALG_SHA256;
  }

  if (mConfig.PcrBankSha384) {
    Banks |= EFI_TCG2_BOOT_HASH_ALG_SHA384;
  }

  return Banks;
}

STATIC
VOID
SetConfigPcrBanks (
  IN UINT32  Banks
  )
{
  mConfig.PcrBankSha1    = (Banks & EFI_TCG2_BOOT_HASH_ALG_SHA1) != 0;
  mConfig.PcrBankSha256  = (Banks & EFI_TCG2_BOOT_HASH_ALG_SHA256) != 0;
  mConfig.PcrBankSha384  = (Banks & EFI_TCG2_BOOT_HASH_ALG_SHA384) != 0;
  mConfig.ActivePcrBanks = Banks;
}

STATIC
VOID
UpdateStrings (
  VOID
  )
{
  CONST CHAR16  *Status;

  if (mHiiHandle == NULL) {
    return;
  }

  if (!mConfig.Enabled) {
    Status = L"Disabled";
  } else if (!mInfo.PreOsAccess || mInfo.TpmReady) {
    Status = L"Enabled";
  } else {
    Status = L"Not responding";
  }

  HiiSetString (mHiiHandle, STRING_TOKEN (STR_STATUS_VALUE), (EFI_STRING)Status, NULL);
  HiiSetString (
    mHiiHandle,
    STRING_TOKEN (STR_STORAGE_VALUE),
    mInfo.PreOsAccess ? L"eMMC RPMB" : L"OS file system (tee-supplicant)",
    NULL
    );
}

STATIC
VOID
UpdateTpmInfoStrings (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT32      ManufacturerId;
  UINT32      FirmwareVersion1;
  UINT32      FirmwareVersion2;
  CHAR16      Buffer[32];
  UINTN       Index;
  CHAR8       Char;

  Status = Tpm2GetCapabilityManufactureID (&ManufacturerId);
  if (!EFI_ERROR (Status)) {
    for (Index = 0; Index < 4; Index++) {
      Char          = (CHAR8)(ManufacturerId >> (Index * 8));
      Buffer[Index] = ((Char >= 0x20) && (Char < 0x7F)) ? Char : L' ';
    }

    Buffer[Index] = L'\0';
    HiiSetString (mHiiHandle, STRING_TOKEN (STR_MANUFACTURER_VALUE), Buffer, NULL);
  }

  Status = Tpm2GetCapabilityFirmwareVersion (&FirmwareVersion1, &FirmwareVersion2);
  if (!EFI_ERROR (Status)) {
    UnicodeSPrint (Buffer, sizeof (Buffer), L"%08X.%08X", FirmwareVersion1, FirmwareVersion2);
    HiiSetString (mHiiHandle, STRING_TOKEN (STR_FIRMWARE_VALUE), Buffer, NULL);
  }
}

STATIC
EFI_STATUS
ClearTpm (
  VOID
  )
{
  EFI_STATUS  Status;

  Status = Tpm2ClearControl (TPM_RH_PLATFORM, NULL, NO);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return Tpm2Clear (TPM_RH_PLATFORM, NULL);
}

STATIC
VOID
RunPendingOperation (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT8       Operation;

  Operation = mConfig.Operation;
  if (Operation == FTPM_OPERATION_NONE) {
    return;
  }

  // Clear the request first so a failing operation cannot repeat on every boot
  mConfig.Operation = FTPM_OPERATION_NONE;
  SaveConfig ();

  switch (Operation) {
    case FTPM_OPERATION_CLEAR:
      Status = ClearTpm ();
      break;
    case FTPM_OPERATION_CHANGE_EPS:
      Status = Tpm2ChangeEPS (TPM_RH_PLATFORM, NULL);
      break;
    default:
      Status = EFI_UNSUPPORTED;
      break;
  }

  DEBUG ((EFI_ERROR (Status) ? DEBUG_ERROR : DEBUG_INFO, "%a: Operation %u: %r\n", __func__, Operation, Status));
}

STATIC
VOID
SyncPcrBanks (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT32      Supported;
  UINT32      Active;
  UINT32      Wanted;

  Status = Tpm2GetCapabilitySupportedAndActivePcrs (&Supported, &Active);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to get PCR banks: %r\n", __func__, Status));
    return;
  }

  // ActivePcrBanks is what the TPM had last boot, so only a change made in setup triggers an allocation
  Wanted = GetConfigPcrBanks () & Supported;
  if ((mConfig.ActivePcrBanks != 0) && (Wanted != 0) &&
      (Wanted != mConfig.ActivePcrBanks) && (Wanted != Active))
  {
    Status = Tpm2PcrAllocateBanks (NULL, Supported, Wanted);
    DEBUG ((EFI_ERROR (Status) ? DEBUG_ERROR : DEBUG_INFO, "%a: Allocate 0x%x: %r\n", __func__, Wanted, Status));
    if (!EFI_ERROR (Status)) {
      SetConfigPcrBanks (Wanted);
      SaveConfig ();
      mResetPending = TRUE;
      return;
    }
  }

  if ((GetConfigPcrBanks () != Active) || (mConfig.ActivePcrBanks != Active)) {
    SetConfigPcrBanks (Active);
    SaveConfig ();
  }
}

STATIC
VOID
EFIAPI
OnResetPending (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);
}

STATIC
VOID
EFIAPI
OnEndOfDxe (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS  Status;
  VOID        *Tcg2;
  EFI_EVENT   ResetEvent;

  gBS->CloseEvent (Event);

  if (mConfig.Enabled && mInfo.PreOsAccess) {
    Status = gBS->LocateProtocol (&gEfiTcg2ProtocolGuid, NULL, &Tcg2);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "%a: fTPM is not responding\n", __func__));
    } else {
      RunPendingOperation ();
      SyncPcrBanks ();
      UpdateTpmInfoStrings ();
      ConfigureTpmPlatformHierarchy ();
      mInfo.TpmReady = TRUE;
    }
  }

  if (mResetPending) {
    // Queued behind RkFvbDxe's EndOfDxe handler, which must find its disk before the reset flushes variables
    Status = gBS->CreateEvent (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, OnResetPending, NULL, &ResetEvent);
    ASSERT_EFI_ERROR (Status);
    gBS->SignalEvent (ResetEvent);
  }

  PtaInvoke (PTA_RK_FTPM_CMD_LOCK, NULL);
  PtaClose ();

  SaveInfo ();
  UpdateStrings ();
}

STATIC
EFI_STATUS
InstallHiiPages (
  VOID
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  DriverHandle;

  DriverHandle = NULL;
  Status       = gBS->InstallMultipleProtocolInterfaces (
                        &DriverHandle,
                        &gEfiDevicePathProtocolGuid,
                        &mVendorDevicePath,
                        NULL
                        );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  mHiiHandle = HiiAddPackages (
                 &gFtpmConfigFormSetGuid,
                 DriverHandle,
                 FtpmConfigDxeStrings,
                 FtpmConfigDxeHiiBin,
                 NULL
                 );
  if (mHiiHandle == NULL) {
    gBS->UninstallMultipleProtocolInterfaces (
           DriverHandle,
           &gEfiDevicePathProtocolGuid,
           &mVendorDevicePath,
           NULL
           );
    return EFI_OUT_OF_RESOURCES;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
FtpmConfigInitialize (
  IN  EFI_HANDLE        ImageHandle,
  IN  EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  UINT32      Flags;
  EFI_EVENT   Event;
  EFI_HANDLE  Handle;

  if (!IsOpteePresent ()) {
    return EFI_UNSUPPORTED;
  }

  Status = PtaOpen ();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "%a: No fTPM control PTA: %r\n", __func__, Status));
    return EFI_UNSUPPORTED;
  }

  Status = PtaInvoke (PTA_RK_FTPM_CMD_GET_INFO, &Flags);
  if (EFI_ERROR (Status) || ((Flags & PTA_RK_FTPM_EMBEDDED) == 0)) {
    PtaClose ();
    return EFI_UNSUPPORTED;
  }

  LoadConfig ();
  mInfo.PreOsAccess = (Flags & PTA_RK_FTPM_STORAGE_RPMB) != 0;
  mInfo.TpmReady    = FALSE;

  if (!mConfig.Enabled) {
    PtaInvoke (PTA_RK_FTPM_CMD_DISABLE, NULL);
  } else if (mInfo.PreOsAccess) {
    // Without PEI nothing syncs the hash mask to the active PCR banks, so use last boot's banks
    if (mConfig.ActivePcrBanks != 0) {
      Status = PcdSet32S (PcdTpm2HashMask, mConfig.ActivePcrBanks);
      ASSERT_EFI_ERROR (Status);
      Status = PcdSet32S (PcdTcg2HashAlgorithmBitmap, mConfig.ActivePcrBanks);
      ASSERT_EFI_ERROR (Status);
    }

    Handle = NULL;
    Status = gBS->InstallMultipleProtocolInterfaces (
                    &Handle,
                    &gRockchipFtpmStorageRequestProtocolGuid,
                    NULL,
                    NULL
                    );
    ASSERT_EFI_ERROR (Status);
  }

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  OnEndOfDxe,
                  NULL,
                  &gEfiEndOfDxeEventGroupGuid,
                  &Event
                  );
  ASSERT_EFI_ERROR (Status);

  SaveInfo ();

  Status = InstallHiiPages ();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to install HII pages: %r\n", __func__, Status));
  }

  UpdateStrings ();

  return EFI_SUCCESS;
}
