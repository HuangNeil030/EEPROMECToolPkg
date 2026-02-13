## @file
#  EEPROMECToolPkg.dsc
##

[Defines]
  PLATFORM_NAME                  = EEPROMECTool
  PLATFORM_GUID                  = 8a9d16e1-2345-6789-abcd-ef0123456789
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/EEPROMECToolPkg
  SUPPORTED_ARCHITECTURES        = X64
  BUILD_TARGETS                  = DEBUG|RELEASE
  SKUID_IDENTIFIER               = DEFAULT

[Packages]
  MdePkg/MdePkg.dec
  MdeModulePkg/MdeModulePkg.dec
  EmulatorPkg/EmulatorPkg.dec
  ShellPkg/ShellPkg.dec
  EEPROMECAppPkg/EEPROMECAppPkg.dec
  

[LibraryClasses]
  DebugLib                      | MdePkg/Library/BaseDebugLibNull/BaseDebugLibNull.inf
  DebugPrintErrorLevelLib       | MdePkg/Library/BaseDebugPrintErrorLevelLib/BaseDebugPrintErrorLevelLib.inf
  RegisterFilterLib             | MdePkg/Library/RegisterFilterLibNull/RegisterFilterLibNull.inf
  PcdLib                        | MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf

  UefiLib                       | MdePkg/Library/UefiLib/UefiLib.inf
  UefiApplicationEntryPoint     | MdePkg/Library/UefiApplicationEntryPoint/UefiApplicationEntryPoint.inf
  UefiBootServicesTableLib      | MdePkg/Library/UefiBootServicesTableLib/UefiBootServicesTableLib.inf
  BaseLib                       | MdePkg/Library/BaseLib/BaseLib.inf
  BaseMemoryLib                 | MdePkg/Library/BaseMemoryLib/BaseMemoryLib.inf
  PrintLib                      | MdePkg/Library/BasePrintLib/BasePrintLib.inf
  IoLib                         | MdePkg/Library/BaseIoLibIntrinsic/BaseIoLibIntrinsic.inf
  StackCheckLib | MdePkg/Library/StackCheckLibNull/StackCheckLibNull.inf
  MemoryAllocationLib | MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf
  DevicePathLib | MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf
  UefiRuntimeServicesTableLib | MdePkg/Library/UefiRuntimeServicesTableLib/UefiRuntimeServicesTableLib.inf
  ShellCEntryLib|ShellPkg/Library/UefiShellCEntryLib/UefiShellCEntryLib.inf
  ShellLib|ShellPkg/Library/UefiShellLib/UefiShellLib.inf
  BaseMemoryLib|MdePkg/Library/BaseMemoryLib/BaseMemoryLib.inf

[Components]
  EEPROMECToolPkg/Applications/EEPROMECTool/EEPROMECTool.inf