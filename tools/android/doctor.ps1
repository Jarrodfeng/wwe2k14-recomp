<#
.SYNOPSIS
    Checks the Android tools and the phone link, and writes the phone's facts (ANDROID_PLAN.md step 1.5).

.DESCRIPTION
    1. Tool versions (JDK, sdkmanager, NDK, build-tools, adb, scrcpy); lists what is missing.
    2. `adb devices -l`.
    3. With one phone connected: model, Android version, API level, SoC, page size, memory, screen, refresh rates and
       the Vulkan driver's features (`cmd gpu vkjson`) into out\android\phone_facts.txt (raw JSON next to it as
       vkjson.json). Read-only on the phone. Works with nothing installed: it just reports what is missing.

.EXAMPLE
    pwsh -File tools\android\doctor.ps1
#>
. "$PSScriptRoot\env.ps1" -AllowMissing
$ErrorActionPreference = "Continue"

function Say([string]$Label, $Value) { "{0,-14} {1}" -f $Label, $Value }

"== Android doctor, $(Get-Date -Format 'yyyy-MM-dd HH:mm')"
Say "SDK" $AndroidSdk
Say "JDK" $(if ($Jdk) { "$Jdk ($((& "$Jdk\bin\java.exe" -version 2>&1 | Select-Object -First 1)))" } else { "missing" })
Say "NDK" $(if ($Ndk) { Split-Path -Leaf $Ndk } else { "missing" })
Say "build-tools" $(if ($BuildTools) { Split-Path -Leaf $BuildTools } else { "missing" })
Say "android.jar" $(if (Test-Path $AndroidJar) { "android-$ApiLevel" } else { "missing" })
Say "adb" $(if (Test-Path $Adb) { (& $Adb version | Select-Object -First 1) } else { "missing" })
Say "scrcpy" $(if ($Scrcpy) { $Scrcpy } else { "missing (optional)" })
Say "package" $Pkg
if ($AndroidMissing.Count) { "MISSING: $($AndroidMissing -join '; ')" }
if (-not (Test-Path $Adb)) { return }

""
"== adb devices -l"
$devices = & $Adb devices -l | Select-Object -Skip 1 | Where-Object { $_.Trim() }
if (-not $devices) { "no phone found: plug it in, unlock it, accept 'Allow USB debugging?'"; return }
$devices
$ready = @($devices | Where-Object { $_ -match '\sdevice\s' })
if ($devices -match 'unauthorized') { "the phone has not accepted this PC yet: unlock it and tap Allow on 'Allow USB debugging?'" }
if ($ready.Count -ne 1) { "need exactly one ready phone (found $($ready.Count))"; return }

function Sh([string]$Cmd) { (& $Adb shell $Cmd 2>&1 | Out-String).Trim() }

$facts = [System.Collections.Generic.List[string]]::new()
$facts.Add("# Phone facts, $(Get-Date -Format 'yyyy-MM-dd HH:mm') (tools\android\doctor.ps1)")
foreach ($p in 'ro.product.manufacturer', 'ro.product.model', 'ro.product.device', 'ro.build.version.release',
               'ro.build.version.sdk', 'ro.build.display.id', 'ro.soc.manufacturer', 'ro.soc.model',
               'ro.product.cpu.abilist', 'ro.hardware.vulkan', 'ro.hardware.egl') {
    $facts.Add(("{0,-26} {1}" -f $p, (Sh "getprop $p")))
}
$facts.Add(("{0,-26} {1}" -f 'PAGE_SIZE', (Sh 'getconf PAGE_SIZE')))
$facts.Add(("{0,-26} {1}" -f 'wm size', ((Sh 'wm size') -replace '\s+', ' ')))
$facts.Add(("{0,-26} {1}" -f 'wm density', ((Sh 'wm density') -replace '\s+', ' ')))
$facts.Add("")
$facts.Add("## /proc/meminfo (first 3 lines)")
(Sh 'head -3 /proc/meminfo') -split "`n" | ForEach-Object { $facts.Add($_.TrimEnd()) }
$facts.Add("")
$facts.Add("## refresh rates (dumpsys display, rough)")
(Sh 'dumpsys display') -split "`n" | Select-String -Pattern 'refreshRate=|supportedRefreshRates|renderFrameRate' |
    Select-Object -First 6 | ForEach-Object { $facts.Add($_.Line.Trim().Substring(0, [Math]::Min(200, $_.Line.Trim().Length))) }

# -- Vulkan driver features ------------------------------------------------------------------------------------------
$facts.Add("")
$facts.Add("## Vulkan (cmd gpu vkjson)")
$vkRaw = Sh 'cmd gpu vkjson'
$vk = $null
try { $vk = $vkRaw | ConvertFrom-Json -Depth 64 } catch { }
if (-not $vk) {
    $facts.Add("cmd gpu vkjson gave no JSON (first line: $(($vkRaw -split "`n")[0])); the game's Vulkan log in phase 5 replaces it")
} else {
    Set-Content -Path "$AndroidOut\vkjson.json" -Value $vkRaw -Encoding utf8
    foreach ($dev in @($vk.devices)) {
        $pr = $dev.properties
        $drv = $pr.driverVersion
        $facts.Add(("device          $($pr.deviceName)  (vendor 0x{0:X4}, device 0x{1:X8})" -f [int64]$pr.vendorID, [int64]$pr.deviceID))
        $facts.Add(("apiVersion      {0}.{1}.{2}" -f ($pr.apiVersion -shr 22), (($pr.apiVersion -shr 12) -band 0x3FF), ($pr.apiVersion -band 0xFFF)))
        $facts.Add(("driverVersion   $drv (0x{0:X8})" -f [int64]$drv))
        $f = $dev.features
        foreach ($n in 'textureCompressionBC', 'vertexPipelineStoresAndAtomics', 'fragmentStoresAndAtomics',
                       'shaderStorageImageWriteWithoutFormat', 'independentBlend', 'depthClamp', 'samplerAnisotropy',
                       'geometryShader', 'fillModeNonSolid', 'shaderClipDistance', 'fullDrawIndexUint32') {
            $facts.Add(("{0,-38} {1}" -f $n, $f.$n))
        }
        $facts.Add(("{0,-38} {1}" -f 'maxStorageBufferRange', $pr.limits.maxStorageBufferRange))
        $facts.Add(("{0,-38} {1}" -f 'maxImageDimension2D', $pr.limits.maxImageDimension2D))
        $exts = @($dev.extensions | ForEach-Object { $_.extensionName })
        foreach ($e in 'VK_EXT_transform_feedback', 'VK_EXT_fragment_shader_interlock', 'VK_EXT_shader_stencil_export',
                       'VK_KHR_dynamic_rendering', 'VK_KHR_portability_subset', 'VK_EXT_custom_border_color',
                       'VK_KHR_shader_float_controls', 'VK_EXT_depth_clip_enable') {
            $facts.Add(("{0,-38} {1}" -f $e, $(if ($exts -contains $e) { 'yes' } else { 'no' })))
        }
        $facts.Add("extensions      $($exts.Count) device extensions (full list in vkjson.json)")
    }
}

Set-Content -Path "$AndroidOut\phone_facts.txt" -Value $facts -Encoding utf8
""
$facts
""
"written: $AndroidOut\phone_facts.txt"
