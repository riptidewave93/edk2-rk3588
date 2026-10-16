/** @file

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef FTPM_CONFIG_DXE_H_
#define FTPM_CONFIG_DXE_H_

#include <Guid/FtpmConfigFormSet.h>

#define FTPM_CONFIG_DATA_VAR_NAME  L"FtpmConfigData"
#define FTPM_INFO_DATA_VAR_NAME    L"FtpmInfoData"

#define FTPM_OPERATION_NONE        0
#define FTPM_OPERATION_CLEAR       1
#define FTPM_OPERATION_CHANGE_EPS  2

#pragma pack (1)

typedef struct {
  BOOLEAN    Enabled;
  UINT8      Operation;
  BOOLEAN    PcrBankSha1;
  BOOLEAN    PcrBankSha256;
  BOOLEAN    PcrBankSha384;
  UINT32     ActivePcrBanks;
} FTPM_CONFIG_VARSTORE_DATA;

typedef struct {
  BOOLEAN    PreOsAccess;
  BOOLEAN    TpmReady;
} FTPM_INFO_VARSTORE_DATA;

#pragma pack ()

#endif // FTPM_CONFIG_DXE_H_
