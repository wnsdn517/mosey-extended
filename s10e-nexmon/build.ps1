# Builds the mosey-s10e KSU module zip (Galaxy S10e, BCM4375B1, nexmon).
#   -Ndk   Android NDK root (r27c tested; default: %LOCALAPPDATA%\Android\Sdk\ndk\android-ndk-r27c)
#
# Inputs: repo root module (MoseyApp, mosey_server, libmosey_daemon_ffi.so, sysconfig payload, ...),
#         nexmon\out\bcmdhd_nex.bin_b1 (nexmon\build_fw.sh), tools\wlmon (WSL aarch64-linux-gnu-gcc).
param(
    [string] $Ndk = (Join-Path $env:LOCALAPPDATA 'Android\Sdk\ndk\android-ndk-r27c')
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$repo = Split-Path $here -Parent
$out = Join-Path $here 'out'
$stage = Join-Path $out 'module'
Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
foreach ($d in 'moseynex', 'firmware', 'tools', 'system\vendor\bin', 'system\vendor\lib64',
               'system\vendor\etc\vintf\manifest', 'system\product\etc\sysconfig') {
    New-Item -ItemType Directory -Force (Join-Path $stage $d) | Out-Null
}

# 1. LD_PRELOAD shim (bionic)
$cc = Join-Path $Ndk 'toolchains\llvm\prebuilt\windows-x86_64\bin\aarch64-linux-android30-clang.cmd'
& $cc -shared -fPIC -O2 -Wall -Wextra -Wno-unused-parameter -o "$stage\moseynex\libmoseynex.so" "$here\moseynex.c" -ldl
if ($LASTEXITCODE) { throw 'shim build failed' }

# 2. mosey_server: radio=1 + ARMv8.2 (retaa/ldapr) patches
python -I "$here\patch_mosey_server.py" "$repo\system\vendor\bin\mosey_server" "$stage\system\vendor\bin\mosey_server"
if ($LASTEXITCODE) { throw 'patch failed' }
Copy-Item "$repo\system\vendor\bin\sqlite3_mosey" "$stage\system\vendor\bin\"
Copy-Item "$repo\system\vendor\lib64\libmosey_daemon_ffi.so" "$stage\system\vendor\lib64\"
Copy-Item "$repo\system\vendor\etc\vintf\manifest\manifest_mosey.xml" "$stage\system\vendor\etc\vintf\manifest\"
# NOT copied: system\vendor\etc\init\mosey.rc (init would start it without the shim) and the Pixel
# vendor_service_contexts (customize.sh derives it from the device's own file).

# 3. MoseyApp, permissions, phenotype patch DB, Pixel experience sysconfig payload
Copy-Item -Recurse "$repo\system\product\priv-app" "$stage\system\product\"
foreach ($d in 'permissions', 'default-permissions', 'sysconfig') {
    Copy-Item -Recurse -Force "$repo\system\product\etc\$d" "$stage\system\product\etc\"
}
# S10 MoseyApp override (newer build in s10e-nexmon\moseyapp; IMoseyService checked identical to
# the v2025_09_23 mosey_server) + matching privapp allowlist (privapp enforce mode).
$moseyApk = Join-Path $here 'moseyapp'
$moseyVc = '16753'
if (Test-Path "$moseyApk\base.apk") {
    $appDir = "$stage\system\product\priv-app\MoseyApp"
    Remove-Item "$appDir\*.apk"
    Copy-Item "$moseyApk\*.apk" $appDir
    $badging = & (Join-Path $env:LOCALAPPDATA 'Android\Sdk\build-tools\37.0.0\aapt2.exe') dump badging "$appDir\base.apk" 2>$null | Select-String "versionCode='(\d+)'"
    if (-not $badging) { throw 'cannot read MoseyApp versionCode' }
    $moseyVc = $badging.Matches[0].Groups[1].Value
}
Copy-Item -Force "$here\module\privapp-permissions-mosey.xml" "$stage\system\product\etc\permissions\"
Write-Host "MoseyApp versionCode $moseyVc"
Copy-Item "$repo\payload\*.xml" "$stage\system\product\etc\sysconfig\"

# 4. nexmon firmware + wlmon
$fw = Join-Path $here 'nexmon\out\bcmdhd_nex.bin_b1'
if (-not (Test-Path $fw)) { throw "missing $fw (run nexmon\build_fw.sh in WSL)" }
Copy-Item $fw "$stage\firmware\"
Copy-Item "$here\tools\wlmon" "$stage\tools\"
# optional: data-path injection kernel module (kmod\, built in WSL against the device kernel)
$ko = Join-Path $here 'kmod\out\mosey_dhd.ko'
if (Test-Path $ko) {
    New-Item -ItemType Directory -Force "$stage\kmod" | Out-Null
    Copy-Item $ko "$stage\kmod\"
    # debug info carries the build machine's paths: never ship it
    & (Join-Path $Ndk 'toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-strip.exe') --strip-debug "$stage\kmod\mosey_dhd.ko"
    if ($LASTEXITCODE) { throw 'strip failed' }
} else { Write-Warning "no ${ko}: module ships without data-path injection" }

# 5. scripts (standalone; mosey_flag.sh writes phenotype flag 45704091)
foreach ($f in 'module.prop', 'customize.sh', 'post-fs-data.sh', 'service.sh', 'mosey_flag.sh', 'monwatch.sh', 'status.sh', 'report.sh', 'action.sh') {
    [IO.File]::WriteAllText("$stage\$f", ([IO.File]::ReadAllText("$here\module\$f") -replace "`r`n", "`n"))
}
# ksud's sepolicy parser rejects some comment text: ship the rules only
$rules = (Get-Content "$here\module\sepolicy.rule") | Where-Object { $_ -notmatch '^\s*(#|$)' }
[IO.File]::WriteAllText("$stage\sepolicy.rule", ($rules -join "`n") + "`n")

# Magisk needs META-INF/com/google/android/{update-binary,updater-script}; KernelSU/APatch ignore it
Copy-Item -Recurse "$repo\META-INF" "$stage\"

# checksums verified by customize.sh before it changes anything (sepolicy.rule is edited there)
$sums = Get-ChildItem -Recurse -File $stage | ForEach-Object {
    $rel = $_.FullName.Substring($stage.Length + 1) -replace '\\', '/'
    if ($rel -notmatch '^(META-INF/|customize\.sh$|sepolicy\.rule$|module\.prop$)') {
        '{0}  {1}' -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower(), $rel
    }
}
[IO.File]::WriteAllText("$stage\sha256sums", ($sums -join "`n") + "`n")

$zip = Join-Path $out 'mosey-s10e.zip'
Remove-Item $zip -ErrorAction SilentlyContinue
Compress-Archive -Path "$stage\*" -DestinationPath $zip
Write-Host "built $zip"
