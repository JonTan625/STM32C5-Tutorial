param([Parameter(Mandatory=$true)][string]$ProjectRoot,
 [ValidateSet('boot','app')][string]$Role='boot',
 [ValidateSet('Debug','Release')][string]$Config='Debug',
 [ValidateSet(1,2)][int]$Version=1,
 [string]$ToolsRoot='%LOCALAPPDATA%/stm32cube/bundles')
$ErrorActionPreference='Stop'
$root=(Resolve-Path -LiteralPath $ProjectRoot).Path
$env:PATH="$ToolsRoot/gnu-tools-for-stm32/14.3.1+st.2/bin;$ToolsRoot/ninja/1.13.2+st.1/bin;$ToolsRoot/cmake/4.4.0+st.1/bin;$env:PATH"
$rel=if($Role -eq 'boot'){'examples/middleware/open_bootloader/demo'}else{'examples/hal/gpio/toggle'}
$project=Join-Path $root "$rel/NUCLEO-C542RC/cmake"
$preset="$($Config.ToLower())_GCC_NUCLEO-C542RC"
$tag="${Role}_v${Version}_$($Config.ToLower())"
$dest=Join-Path $root "out43/$tag"
New-Item -ItemType Directory -Force -Path $dest | Out-Null
Push-Location -LiteralPath $project
try {
 $extra=@();if($Role -eq 'app'){$extra=@("-DC5_APP_VERSION=$Version")}
 & cmake --preset $preset -DCMAKE_EXPORT_COMPILE_COMMANDS=ON @extra *> "$dest/configure.log"
 if($LASTEXITCODE -ne 0){throw "configure failed: $dest/configure.log"}
 & cmake --build --preset $preset *> "$dest/build.log"
 if($LASTEXITCODE -ne 0){Get-Content "$dest/build.log" -Tail 20;throw 'build failed'}
 if(Select-String -LiteralPath "$dest/build.log" -Pattern '(?i)warning:'){throw 'compiler warnings'}
 $elf=Get-ChildItem -LiteralPath "build/$preset" -Filter '*.elf' | Select-Object -First 1
 Copy-Item -LiteralPath $elf.FullName -Destination "$dest/firmware.elf"
 foreach($fmt in @(@('binary','bin'),@('ihex','hex'))){
  & arm-none-eabi-objcopy -O $fmt[0] $elf.FullName "$dest/firmware.$($fmt[1])"
  if($LASTEXITCODE -ne 0){throw 'objcopy failed'}
 }
 & arm-none-eabi-size $elf.FullName | Tee-Object -FilePath "$dest/size.txt"
 if($LASTEXITCODE -ne 0){throw 'size failed'}
 Get-ChildItem -LiteralPath "build/$preset" -Filter '*.map' | Copy-Item -Destination $dest
 Write-Output "${tag}：构建完成，0编译警告；$dest；未烧录"
} finally {Pop-Location}
