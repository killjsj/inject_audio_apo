#Requires -RunAsAdministrator
param(
    [string]$DllPath = "",
    [switch]$SkipRegister,
    [switch]$SkipBind,
    [string]$OriginalApoClsid = ""
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$buildRoot = [System.IO.Path]::GetFullPath((Join-Path $root '.\build'))
if (-not $DllPath) {
    $DllPath = Join-Path $buildRoot '\bin\x64\Debug\InjectAudioApo.dll'
}

$ApoClsid = '{5F2EC245-A357-4858-80A2-5E575C904D6F}'
$DefaultProcessingMode = '{C18E2F7E-933D-4965-B7D1-1EEF228D2AF3}'
$CaptureSubKey = 'SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture'
$CaptureBase = 'HKLM:\' + $CaptureSubKey
$EndpointEffectValue = '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},7'
$EfxModesValue = '{d3993a3f-99c2-4402-b5ec-a92a0367664b},7'

$certName = 'CN=InjectAudio Dev Test'
$signtool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" -Recurse -Filter signtool.exe |
    Where-Object { $_.FullName -match '\\x64\\signtool.exe$' } |
    Sort-Object FullName -Descending |
    Select-Object -First 1 -ExpandProperty FullName

if (-not $signtool) { throw "signtool.exe not found (Windows SDK)" }
if (-not (Test-Path $DllPath)) { throw "DLL not found: $DllPath" }

Write-Host "DLL: $DllPath"
Write-Host "signtool: $signtool"

$cert = Get-ChildItem Cert:\LocalMachine\My | Where-Object { $_.Subject -eq $certName } | Select-Object -First 1
if (-not $cert) {
    Write-Host "Creating self-signed code signing certificate..."
    $cert = New-SelfSignedCertificate `
        -Subject $certName `
        -Type CodeSigningCert `
        -CertStoreLocation Cert:\LocalMachine\My `
        -KeyExportPolicy Exportable `
        -KeySpec Signature `
        -HashAlgorithm SHA256 `
        -NotAfter (Get-Date).AddYears(5)
}

$thumb = $cert.Thumbprint
foreach ($storeName in @('Root', 'TrustedPublisher')) {
    $store = New-Object System.Security.Cryptography.X509Certificates.X509Store($storeName, 'LocalMachine')
    $store.Open('ReadWrite')
    $exists = $store.Certificates | Where-Object { $_.Thumbprint -eq $thumb }
    if (-not $exists) {
        $store.Add($cert)
        Write-Host "Added cert to LocalMachine\$storeName"
    } else {
        Write-Host "Cert already in LocalMachine\$storeName"
    }
    $store.Close()
}

try {
    $fs = [IO.File]::Open($DllPath, 'Open', 'ReadWrite', 'None')
    $fs.Close()
} catch {
    $bak = "$DllPath.locked_$(Get-Date -Format 'HHmmss')"
    Write-Host "DLL locked - renaming to $bak"
    Move-Item -LiteralPath $DllPath -Destination $bak -Force
    throw "Rebuild InjectAudio.dll then re-run this script (file was locked by audiodg)."
}

Write-Host "Signing..."
& $signtool sign /fd SHA256 /sm /s My /n "InjectAudio Dev Test" /tr http://timestamp.digicert.com /td SHA256 $DllPath
if ($LASTEXITCODE -ne 0) {
    Write-Host "Timestamp failed, signing without timestamp..."
    & $signtool sign /fd SHA256 /sm /s My /n "InjectAudio Dev Test" $DllPath
    if ($LASTEXITCODE -ne 0) { throw "signtool failed" }
}

Get-AuthenticodeSignature $DllPath | Format-List Status, SignerCertificate

if (-not ('KwsFx' -as [type])) {
    Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class KwsFx
{
    const int KEY_QUERY_VALUE = 0x0001;
    const int KEY_SET_VALUE = 0x0002;
    const int KEY_READ = 0x20019;
    const uint REG_SZ = 1;
    const uint REG_MULTI_SZ = 7;
    const int ERROR_SUCCESS = 0;
    const int ERROR_FILE_NOT_FOUND = 2;
    const int ERROR_PATH_NOT_FOUND = 3;
    const int ERROR_MORE_DATA = 234;
    static readonly IntPtr HKLM = new IntPtr(unchecked((int)0x80000002));

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern int RegCreateKeyExW(IntPtr hKey, string lpSubKey, int reserved, string lpClass,
        int dwOptions, int samDesired, IntPtr lpSecurityAttributes,
        out IntPtr hkResult, out int lpdwDisposition);

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern int RegOpenKeyExW(IntPtr hKey, string lpSubKey, int ulOptions, int samDesired,
        out IntPtr hkResult);

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern int RegSetValueExW(IntPtr hKey, string valueName, int reserved, uint dwType,
        byte[] lpData, int cbData);

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern int RegQueryValueExW(IntPtr hKey, string valueName, IntPtr reserved,
        out uint dwType, StringBuilder lpData, ref int lpcbData);

    [DllImport("advapi32.dll", SetLastError = true)]
    static extern int RegCloseKey(IntPtr hKey);

    static string Subkey(string relativePath)
    {
        return relativePath.Replace("HKLM:\\", "");
    }

    public static int SetStringValue(string keyPath, string valueName, string value)
    {
        return SetValueInternal(keyPath, valueName, REG_SZ, Encoding.Unicode.GetBytes(value + "\0"));
    }

    public static int SetMultiStringValue(string keyPath, string valueName, string[] values)
    {
        StringBuilder sb = new StringBuilder();
        foreach (string v in values) { sb.Append(v); sb.Append('\0'); }
        sb.Append('\0');
        return SetValueInternal(keyPath, valueName, REG_MULTI_SZ, Encoding.Unicode.GetBytes(sb.ToString()));
    }

    static int SetValueInternal(string keyPath, string valueName, uint type, byte[] data)
    {
        string sub = Subkey(keyPath);
        IntPtr key;
        int disposition;
        int status = RegCreateKeyExW(HKLM, sub, 0, null, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, IntPtr.Zero, out key, out disposition);
        if (status != ERROR_SUCCESS) return status;
        try
        {
            status = RegSetValueExW(key, valueName, 0, type, data, data.Length);
        }
        finally { RegCloseKey(key); }
        return status;
    }

    public static string QueryStringValue(string keyPath, string valueName)
    {
        string sub = Subkey(keyPath);
        IntPtr key;
        int status = RegOpenKeyExW(HKLM, sub, 0, KEY_QUERY_VALUE, out key);
        if (status != ERROR_SUCCESS) return null;
        try
        {
            uint type;
            int size = 0;
            status = RegQueryValueExW(key, valueName, IntPtr.Zero, out type, null, ref size);
            if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) return null;
            if (status == ERROR_MORE_DATA || status == ERROR_SUCCESS)
            {
                StringBuilder sb = new StringBuilder(size + 2);
                status = RegQueryValueExW(key, valueName, IntPtr.Zero, out type, sb, ref size);
                if (status == ERROR_SUCCESS && type == REG_SZ) return sb.ToString();
            }
            return null;
        }
        finally { RegCloseKey(key); }
    }
}
"@
}

function Get-EndpointFxValue([string]$endpointId, [string]$valueName) {
    $path = 'HKLM:\' + $CaptureSubKey + '\' + $endpointId + '\FxProperties'
    $value = [KwsFx]::QueryStringValue($path, $valueName)
    if ($null -eq $value) { return '' }
    return [string]$value
}

function Set-EndpointFxValue([string]$endpointId, [string]$valueName, [string]$value, [string]$type) {
    $path = 'HKLM:\' + $CaptureSubKey + '\' + $endpointId + '\FxProperties'
    if ($type -eq 'REG_MULTI_SZ') {
        $status = [KwsFx]::SetMultiStringValue($path, $valueName, @($value))
    } else {
        $status = [KwsFx]::SetStringValue($path, $valueName, $value)
    }
    if ($status -ne 0) { throw ("set {0} {1} failed: Win32 error {2}" -f $path, $valueName, $status) }
}

function Set-EndpointOriginalApo([string]$endpointId, [string]$value) {
    $path = 'HKLM:\' + $CaptureSubKey + '\' + $endpointId + '\InjectAudio'
    $status = [KwsFx]::SetStringValue($path, 'OriginalApoClsid', $value)
    if ($status -ne 0) { throw ("set {0} OriginalApoClsid failed: Win32 error {1}" -f $path, $status) }
}

function Bind-CaptureEndpoints {
    if (-not (Test-Path -LiteralPath $CaptureBase)) { return }
    $failures = 0
    foreach ($endpoint in Get-ChildItem -LiteralPath $CaptureBase) {
        if (-not $endpoint.PSIsContainer -or $endpoint.PSChildName -notmatch '^\{[0-9a-fA-F-]+\}$') { continue }
        $id = $endpoint.PSChildName
        try {
            if ($OriginalApoClsid) {
                Set-EndpointOriginalApo $id $OriginalApoClsid
                Write-Host ("  OriginalApoClsid = {0}" -f $OriginalApoClsid)
            } else {
                $existing = Get-EndpointFxValue $id $EndpointEffectValue
                if ($existing -and $existing -ine $ApoClsid) {
                    Set-EndpointOriginalApo $id $existing
                    Write-Host ("  OriginalApoClsid <- {0}" -f $existing)
                }
            }

            Set-EndpointFxValue $id $EndpointEffectValue $ApoClsid 'REG_SZ'
            Set-EndpointFxValue $id $EfxModesValue $DefaultProcessingMode 'REG_MULTI_SZ'
            Write-Host ("  [BOUND] {0}" -f $id)
        } catch {
            $failures++
            Write-Host ("  [FAILED] {0} - {1}" -f $id, $_.Exception.Message)
        }
    }
    if ($failures -gt 0) { Write-Host "$failures endpoint(s) could not be updated." }
}

if (-not $SkipRegister) {
    Write-Host "regsvr32..."
    $p = Start-Process regsvr32 -ArgumentList "/s `"$DllPath`"" -Wait -PassThru
    if ($p.ExitCode -ne 0) { throw "regsvr32 failed: $($p.ExitCode)" }

    $clsid = 'HKLM:\SOFTWARE\Classes\CLSID\{5F2EC245-A357-4858-80A2-5E575C904D6F}\InprocServer32'
    Set-ItemProperty -LiteralPath $clsid -Name '(default)' -Value $DllPath
    Write-Host "InprocServer32 0AC3 -> $DllPath"
}

if (-not $SkipBind) {
    Write-Host "Binding APO on every capture endpoint..."
    Bind-CaptureEndpoints
}

if (-not $SkipRegister) {
    Write-Host "Restart audio stack..."
    Stop-Service Audiosrv -Force -EA SilentlyContinue
    Stop-Service AudioEndpointBuilder -Force -EA SilentlyContinue
    Start-Sleep 1
    Get-Process audiodg -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue
    Start-Sleep 1
    Start-Service AudioEndpointBuilder -EA SilentlyContinue
    Start-Service Audiosrv -EA SilentlyContinue
}

Write-Host ""
Write-Host "Done. Keep SysFx ON."
