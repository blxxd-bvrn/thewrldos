# Build to ISO without WSL or Docker (run by "Build to ISO.bat"; Windows 10 or 11, no administrator
# rights needed). Two ways:
#
#   -Mode download   fetch the ISOs GitHub Actions built from the repository (the newest push to the
#                    main branch; your own changes are not in them)
#   -Mode vm         build your sources in a portable Linux VM: the first time it fetches the build
#                    kit (an Ubuntu disk with every build tool, and QEMU for Windows) into
#                    %LOCALAPPDATA%\WRLD OS build VM, then it builds there. With "Windows Hypervisor
#                    Platform" turned on (Windows Features) QEMU runs at full speed; without it the
#                    whole computer is emulated (a full build took 25 minutes on four processors).
#
# Both use only what Windows has: PowerShell, curl.exe and tar.exe.
param(
    [Parameter(Mandatory = $true)][ValidateSet('download', 'vm')][string]$Mode,
    [Parameter(Mandatory = $true)][string]$Src,
    [string]$Repo = '',
    [string]$KitDir = '',          # where the build kit lives (default: %LOCALAPPDATA%\WRLD OS build VM)
    [switch]$NoDownload,           # use the kit already in KitDir (the kit workflow's own test)
    [switch]$Emulate,              # never use Windows Hypervisor Platform (a computer where it hangs)
    [switch]$Verbose               # show the Linux kernel's boot messages too
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

function Say($m) { Write-Host "==> $m" -ForegroundColor Cyan }
function Fail($m) { Write-Host "ERROR: $m" -ForegroundColor Red; exit 1 }

# ---- which GitHub repository: from .git\config, else tools\buildvm\repo.txt -------------------
if (-not $Repo) {
    $cfg = Join-Path $Src '.git\config'
    if (Test-Path $cfg) {
        $m = Select-String -Path $cfg -Pattern 'url\s*=\s*(?:https://github\.com/|git@github\.com:)([^/\s]+/[^/\s]+?)(?:\.git)?\s*$' | Select-Object -First 1
        if ($m) { $Repo = $m.Matches[0].Groups[1].Value }
    }
}
if (-not $Repo) {
    $rf = Join-Path $Src 'tools\buildvm\repo.txt'
    if (Test-Path $rf) { $Repo = (Get-Content $rf | Where-Object { $_ -match '^\s*[^#\s]' } | Select-Object -First 1).Trim() }
}
if (-not $Repo -and -not $NoDownload) { Fail "could not tell which GitHub repository to use. Put OWNER/NAME in tools\buildvm\repo.txt." }

$curl = Join-Path $env:SystemRoot 'System32\curl.exe'
$tar = Join-Path $env:SystemRoot 'System32\tar.exe'
if (-not (Test-Path $curl) -or -not (Test-Path $tar)) { Fail "this needs Windows 10 (version 1803) or newer, which include curl.exe and tar.exe." }

function Fetch($url, $dest) {
    & $curl -L -f --retry 3 --progress-bar -o $dest $url
    if ($LASTEXITCODE -ne 0) { Fail "could not download $url" }
}
function Sha256($f) { (Get-FileHash -Algorithm SHA256 $f).Hash.ToLower() }

# ---- 1. ready-made ISOs ---------------------------------------------------------------------
if ($Mode -eq 'download') {
    $base = "https://github.com/$Repo/releases/download/latest-iso"
    $tmp = Join-Path $env:TEMP "wrldos-iso-$PID"
    New-Item -ItemType Directory -Force $tmp | Out-Null
    Say "Downloading the ISOs GitHub built for $Repo"
    Fetch "$base/built-from.txt" (Join-Path $tmp 'built-from.txt')
    Get-Content (Join-Path $tmp 'built-from.txt') | ForEach-Object { Write-Host "    $_" }
    foreach ($f in 'wrldos.iso', 'wrldos64.iso') {
        Say $f
        Fetch "$base/$f" (Join-Path $tmp $f)
    }
    Fetch "$base/SHA256SUMS" (Join-Path $tmp 'SHA256SUMS')
    foreach ($line in Get-Content (Join-Path $tmp 'SHA256SUMS')) {
        $h, $n = $line -split '\s+\*?', 2
        if ($n -and (Test-Path (Join-Path $tmp $n)) -and (Sha256 (Join-Path $tmp $n)) -ne $h.ToLower()) { Fail "$n did not download correctly (checksum mismatch); try again." }
    }
    foreach ($f in 'wrldos.iso', 'wrldos64.iso') { Move-Item -Force (Join-Path $tmp $f) (Join-Path $Src $f) }
    Remove-Item -Recurse -Force $tmp
    exit 0
}

# ---- 2. the portable build VM ---------------------------------------------------------------
$kit = if ($KitDir) { $KitDir } else { Join-Path $env:LOCALAPPDATA 'WRLD OS build VM' }
New-Item -ItemType Directory -Force $kit | Out-Null
$base = "https://github.com/$Repo/releases/download/build-vm-kit"
$manifest = Join-Path $kit 'kit.txt'
$newManifest = Join-Path $kit 'kit.new.txt'

Say "Checking the build kit"
if ($NoDownload) { Copy-Item (Join-Path $kit 'kit.txt') $newManifest } else {
& $curl -L -f -s --retry 3 -o $newManifest "$base/kit.txt"
if ($LASTEXITCODE -ne 0) {
    if (-not (Test-Path $manifest)) { Fail "could not download the build kit from $base (has the 'Build VM kit' workflow run on GitHub?)" }
    Write-Host "    (offline: using the kit already here)"
    Copy-Item $manifest $newManifest
}
}
# kit.txt: "version <date>", then "<sha256> <file>" for each file; a disk image too big for one
# GitHub file comes in parts named <image>.partNN, joined here
$entries = @(); $version = ''
foreach ($line in Get-Content $newManifest) {
    if ($line -match '^version\s+(.+)$') { $version = $Matches[1].Trim() }
    elseif ($line -match '^([0-9a-f]{64})\s+(\S+)$') { $entries += [pscustomobject]@{ Hash = $Matches[1]; Name = $Matches[2] } }
}
$have = if (Test-Path $manifest) { (Get-Content $manifest | Where-Object { $_ -match '^version' }) -replace '^version\s+', '' } else { '' }
$qemuDir = Join-Path $kit 'qemu'
$image = Join-Path $kit 'wrld-build-vm.qcow2'
$kernel = Join-Path $kit 'vmlinuz'
$overlay = Join-Path $kit 'work.qcow2'
if ($have -ne $version -or -not (Test-Path $image) -or -not (Test-Path $kernel) -or -not (Test-Path (Join-Path $qemuDir 'qemu-system-x86_64.exe'))) {
    $total = 0
    Say "Downloading the build kit ($version). This happens once; it is large."
    foreach ($e in $entries) {
        $dest = Join-Path $kit $e.Name
        if ((Test-Path $dest) -and (Sha256 $dest) -eq $e.Hash) { continue }
        Write-Host "    $($e.Name)"
        Fetch "$base/$($e.Name)" $dest
        if ((Sha256 $dest) -ne $e.Hash) { Fail "$($e.Name) did not download correctly (checksum mismatch); run Build to ISO again." }
    }
    $parts = $entries | Where-Object { $_.Name -like 'wrld-build-vm.qcow2.part*' } | Sort-Object Name
    if ($parts) {
        Say "Joining the disk image"
        $out = [IO.File]::Create($image)
        foreach ($p in $parts) { $in = [IO.File]::OpenRead((Join-Path $kit $p.Name)); $in.CopyTo($out); $in.Close() }
        $out.Close()
        foreach ($p in $parts) { Remove-Item (Join-Path $kit $p.Name) }
    }
    $zip = $entries | Where-Object { $_.Name -like '*qemu*.zip' } | Select-Object -First 1
    if ($zip) {
        Say "Unpacking QEMU"
        if (Test-Path $qemuDir) { Remove-Item -Recurse -Force $qemuDir }
        New-Item -ItemType Directory -Force $qemuDir | Out-Null
        & $tar -xf (Join-Path $kit $zip.Name) -C $qemuDir
        if ($LASTEXITCODE -ne 0) { Fail "could not unpack $($zip.Name)" }
        Remove-Item (Join-Path $kit $zip.Name)
    }
    if (Test-Path $overlay) { Remove-Item $overlay }      # the work disk sits on the old image
    Copy-Item -Force $newManifest $manifest
}
Remove-Item -Force $newManifest -ErrorAction SilentlyContinue

$qemu = Join-Path $qemuDir 'qemu-system-x86_64.exe'
$qimg = Join-Path $qemuDir 'qemu-img.exe'
if (-not (Test-Path $qemu)) { Fail "QEMU is missing from $qemuDir" }

# the work disk: changes on top of the kit's image, kept between builds (object files make the
# next build faster); deleting it starts afresh
if (-not (Test-Path $overlay)) {
    & $qimg create -q -f qcow2 -F qcow2 -b $image $overlay
    if ($LASTEXITCODE -ne 0) { Fail "could not create the work disk" }
}

# the sources go in as a tar archive on a second disk, the ISOs come out on a third
$srcTar = Join-Path $kit 'sources.tar'
$outImg = Join-Path $kit 'output.img'
Say "Packing the sources"
Remove-Item -Force $srcTar, $outImg -ErrorAction SilentlyContinue
& $tar -cf $srcTar --exclude ./build --exclude ./build64 --exclude ./.git --exclude ./rust/target --exclude '*.iso' --exclude ./disk.img --exclude ./disk64.img -C $Src .
if ($LASTEXITCODE -ne 0) { Fail "could not pack the sources" }
& $qimg create -q -f raw $outImg 3G
if ($LASTEXITCODE -ne 0) { Fail "could not create the output disk" }

$cpus = [Math]::Max(1, [Math]::Min([Environment]::ProcessorCount, 8))
$ramMB = [int]((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1MB)
$mem = [Math]::Max(2048, [Math]::Min(6144, [int]($ramMB / 2)))
Say "Starting the build VM ($cpus processors, $mem MB). The build's own messages follow."
Write-Host "    If QEMU reports that WHPX is not available, it carries on emulating (much slower)."
Write-Host "    Turn on 'Windows Hypervisor Platform' in Windows Features for full speed."
if (-not $Emulate) { Write-Host "    If nothing happens for many minutes (a virtual machine without nested virtualization), run 'Build to ISO.bat' vm-emulate." }
Write-Host ""
# the hypervisor first (QEMU falls back to the next -accel when it is missing)
$accel = @(if (-not $Emulate) { '-accel'; 'whpx,kernel-irqchip=off' })
$qargs = @('-L', (Join-Path $qemuDir 'share'), '-machine', 'q35', '-m', "$mem", '-smp', "$cpus") + $accel + @(
    '-accel', 'tcg,thread=multi',
    '-kernel', $kernel,
    '-append', ('root=/dev/vda rw console=ttyS0 init=/usr/local/sbin/wrld-vm-build panic=-1' + $(if ($Verbose) { '' } else { ' quiet loglevel=3' })),
    '-drive', "file=$overlay,if=virtio,format=qcow2",
    '-drive', "file=$srcTar,if=virtio,format=raw,readonly=on",
    '-drive', "file=$outImg,if=virtio,format=raw",
    '-netdev', 'user,id=n0', '-device', 'virtio-net-pci,netdev=n0,romfile=',
    '-nographic', '-vga', 'none', '-no-reboot'
)
& $qemu @qargs
if ($LASTEXITCODE -ne 0) { Fail "QEMU stopped with an error ($LASTEXITCODE)." }

Say "Collecting the result"
$res = Join-Path $kit 'result'
if (Test-Path $res) { Remove-Item -Recurse -Force $res }
New-Item -ItemType Directory -Force $res | Out-Null
& $tar -xf $outImg -C $res
if (-not (Test-Path (Join-Path $res 'status'))) { Fail "the build VM did not finish (no result on the output disk)." }
Copy-Item -Force (Join-Path $res 'build.log') (Join-Path $Src 'build-vm.log') -ErrorAction SilentlyContinue
$status = (Get-Content (Join-Path $res 'status') | Select-Object -First 1).Trim()
Remove-Item -Force $srcTar, $outImg -ErrorAction SilentlyContinue
if ($status -ne '0') { Fail "the build failed in the VM; its log is in build-vm.log next to Build to ISO.bat." }
foreach ($f in 'wrldos.iso', 'wrldos64.iso') { Move-Item -Force (Join-Path $res $f) (Join-Path $Src $f) }
Remove-Item -Recurse -Force $res
exit 0
