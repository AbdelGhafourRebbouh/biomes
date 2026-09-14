[CmdletBinding()]
param([string]$ChromePath = "$env:ProgramFiles\Google\Chrome\Application\chrome.exe")
$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $ChromePath)) { throw 'Chrome must be installed for this optional smoke test.' }
# Only temporary profiles and HWNDs owned by our browser process are touched.
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('biomes-chrome-' + [Guid]::NewGuid().ToString('N'))
Add-Type @'
using System;
using System.Collections.Generic;
using System.Text;
using System.Runtime.InteropServices;
public static class BiomesChromeSmoke {
 public delegate bool Callback(IntPtr h, IntPtr p);
 [DllImport("user32.dll")] public static extern bool EnumWindows(Callback cb, IntPtr p);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
 [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
 public static Dictionary<IntPtr,string> Find(int pid) {
  var result = new Dictionary<IntPtr,string>();
  EnumWindows((h,p) => { uint owner; GetWindowThreadProcessId(h,out owner); if(owner==pid && IsWindowVisible(h)) { var s=new StringBuilder(1024); GetWindowText(h,s,1024); result[h]=s.ToString(); } return true; },IntPtr.Zero);
  return result;
 }
}
'@
$common = @("--user-data-dir=`"$testRoot`"", '--no-first-run', '--no-default-browser-check',
    '--disable-background-networking', '--disable-sync', '--disable-component-update', '--disable-background-mode')
$browser = $null
function Open-TestChrome([string]$profile) {
    $arguments = $common + @('--new-window', "--profile-directory=`"$profile`"", 'chrome://newtab/')
    Start-Process -FilePath $ChromePath -ArgumentList $arguments -WindowStyle Hidden -PassThru
}
function Wait-TestWindows([int]$count) {
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    do {
        Start-Sleep -Milliseconds 250
        $windows = [BiomesChromeSmoke]::Find($browser.Id)
        $valid = @($windows.Values | Where-Object { $_ -and $_ -ne 'Google Chrome' })
    } while ($valid.Count -lt $count -and [DateTime]::UtcNow -lt $deadline)
    if ($windows.Count -ne $count -or $valid.Count -ne $count) { throw "Expected $count real browser windows; found $($windows.Count) windows and $($valid.Count) browser titles." }
}
function Close-TestChrome {
    if ($null -eq $browser -or $browser.HasExited) { return }
    foreach ($handle in [BiomesChromeSmoke]::Find($browser.Id).Keys) {
        [void][BiomesChromeSmoke]::PostMessage($handle,16,[IntPtr]::Zero,[IntPtr]::Zero)
    }
    if (-not $browser.WaitForExit(15000)) { throw 'Temporary Chrome did not exit cleanly.' }
}
try {
    # Create real profile metadata using Chrome itself, without any accounts.
    $browser = Open-TestChrome 'Default'
    Wait-TestWindows 1
    $null = Open-TestChrome 'Profile 1'
    Wait-TestWindows 2
    Close-TestChrome
    $statePath = Join-Path $testRoot 'Local State'
    $state = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
    if (@($state.profile.info_cache.PSObject.Properties).Count -lt 2) { throw 'Two-profile fixture was not created.' }
    $state.profile | Add-Member -NotePropertyName last_used -NotePropertyValue 'Profile 1' -Force
    $state.browser | Add-Member -NotePropertyName show_profile_picker_on_startup -NotePropertyValue $true -Force
    $state.browser | Add-Member -NotePropertyName profile_picker_shown -NotePropertyValue $true -Force
    [IO.File]::WriteAllText($statePath, ($state | ConvertTo-Json -Depth 100), [Text.UTF8Encoding]::new($false))

    $browser = Open-TestChrome 'Profile 1'
    Wait-TestWindows 1
    $null = Open-TestChrome 'Profile 1'
    Wait-TestWindows 2
    Start-Sleep -Seconds 2
    Wait-TestWindows 2
    Close-TestChrome
    # A biome can queue both missing windows before Chrome has initialized.
    $browser = Open-TestChrome 'Profile 1'
    $null = Open-TestChrome 'Profile 1'
    Wait-TestWindows 2
    Start-Sleep -Seconds 2
    Wait-TestWindows 2
    Write-Output 'PASS: cold, warm and concurrent Chrome launches with two profiles and startup picker enabled produced two persistent browser HWNDs, no chooser.'
} finally {
    Close-TestChrome
    # Leave the isolated fixture for diagnostics; it is never packaged.
    Write-Output "Temporary Chrome fixture: $testRoot"
}
