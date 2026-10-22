param([Parameter(Mandatory=$true)][string]$ProjectRoot,
      [ValidateSet('Debug','Release')][string]$Config='Debug',
      [ValidateSet('prod','test')][string]$Variant='prod',
      [string]$ToolsRoot="$env:LOCALAPPDATA/stm32cube/bundles")
# C5 #48：构建正式版（prod）或测试变体（test：加 T 命令、日志只用 2 页、周期下限 1 s）。只构建，不接板。
$ErrorActionPreference='Stop'
$env:PATH="$ToolsRoot/gnu-tools-for-stm32/14.3.1+st.2/bin;$ToolsRoot/ninja/1.13.2+st.1/bin;$ToolsRoot/cmake/4.4.0+st.1/bin;$env:PATH"
$cmakeDir=Join-Path $ProjectRoot 'examples/hal/adc/c5_48_rtc_logger/NUCLEO-C542RC/cmake'
$preset="$($Config.ToLower())_GCC_NUCLEO-C542RC"
$bdir= if($Variant -eq 'test'){"build/test_$preset"}else{"build/$preset"}
$extra=@()
if($Variant -eq 'test'){$extra+='-DP48_TEST=ON'}
Push-Location -LiteralPath $cmakeDir
try {
  & cmake --preset $preset -B $bdir @extra
  if($LASTEXITCODE -ne 0){throw 'configure failed'}
  # 官方 CMakeLists 的清理列表写死 build/<preset>：测试变体不要加 --clean-first，否则会删掉正式版 ELF
  & cmake --build $bdir
  if($LASTEXITCODE -ne 0){throw 'build failed'}
  $elf=Get-ChildItem -LiteralPath $bdir -Filter '*.elf' | Select-Object -First 1
  & arm-none-eabi-size $elf.FullName
  & arm-none-eabi-objcopy -O binary $elf.FullName (Join-Path $bdir 'firmware.bin')
  Write-Output "ELF: $($elf.FullName)"
} finally {Pop-Location}
