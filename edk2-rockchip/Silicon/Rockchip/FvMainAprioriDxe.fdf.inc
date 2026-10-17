## @file
#
#  Copyright (c) 2023, Mario Bălănică <mariobalanica02@gmail.com>
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
#
##

!if $(RK_STATUS_LED_ENABLE) == TRUE
  INF Silicon/Rockchip/Drivers/StatusLedDxe/StatusLedDxe.inf
!endif

  INF MdeModulePkg/Universal/PCD/Dxe/Pcd.inf

!if $(RK_STMM_ENABLE) == TRUE
  # The UEFI variables live in RPMB, so the eMMC stack cannot wait for the variable arch protocols
  INF ArmPkg/Drivers/CpuDxe/CpuDxe.inf
  # DxeCore connects the eMMC as soon as DwcSdhciDxe registers it, and identification needs Stall()
  INF EmbeddedPkg/MetronomeDxe/MetronomeDxe.inf
  INF MdeModulePkg/Bus/Pci/NonDiscoverablePciDeviceDxe/NonDiscoverablePciDeviceDxe.inf
  INF MdeModulePkg/Bus/Pci/SdMmcPciHcDxe/SdMmcPciHcDxe.inf
  INF MdeModulePkg/Bus/Sd/EmmcDxe/EmmcDxe.inf
  INF Silicon/Rockchip/Drivers/DwcSdhciDxe/DwcSdhciDxe.inf
!endif
