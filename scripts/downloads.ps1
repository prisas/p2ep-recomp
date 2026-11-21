<#
.SYNOPSIS
    Fetches the third-party extras that the package can include: the Real-ESRGAN upscaler and the
    two community texture packs. Dot-sourced by package.ps1.
.NOTES
    Everything is cached in build\downloads (ignored by Git) and checked against a pinned SHA-256
    before use. None of it is part of the repository; the packs are made from the game's artwork and
    belong to their authors (see third_party\CREDITS.md).
#>

# What is fetched. Sha256 is the digest of the file as downloaded (for MEGA: after decryption).
$script:P2Extras = [ordered]@{
    realesrgan = @{
        Title  = "Real-ESRGAN (realesrgan-ncnn-vulkan 2022-04-24)"
        Url    = "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-windows.zip"
        File   = "realesrgan-ncnn-vulkan-20220424-windows.zip"
        Sha256 = "abc02804e17982a3be33675e4d471e91ea374e65b70167abc09e31acb412802d"
    }
    ryuubu = @{
        Title  = "Persona 2: Eternal Punishment UI Upscale, by Ryuubu"
        Mega   = "https://mega.nz/file/ndgwUYjS#30dJKpDToFQu9HX_8BkVhcqERyoK1EdLw58a8D-Jd8o"
        Sha256 = "dd6616591ebcb9b3b2968438f40f75773bd086f4807f2648685e7154318f2172"
    }
    rarno = @{
        Title  = "Eternal Punishment HD Textures 1.03, by Rarno"
        Url    = "https://gamebanana.com/dl/988536"
        File   = "uljm06081_eb645.rar"
        Sha256 = "af5c7f148e5664c87208d83ff4b0aa9c653b13f4c550e7db3ffc30ffad2527c3"
    }
}

# MEGA's public file links: the file is AES-128-CTR encrypted with a key derived from the 256-bit
# key in the link, and its integrity is a CBC-MAC over chunks that must match the link's meta-MAC.
if (-not ("P2Mega" -as [type])) {
    Add-Type -TypeDefinition @"
using System;
using System.IO;
using System.Security.Cryptography;

public static class P2Mega {
    static uint[] Words(byte[] b) {
        var w = new uint[b.Length / 4];
        for (int i = 0; i < w.Length; i++)
            w[i] = (uint)(b[4 * i] << 24 | b[4 * i + 1] << 16 | b[4 * i + 2] << 8 | b[4 * i + 3]);
        return w;
    }
    static byte[] Bytes(params uint[] w) {
        var b = new byte[w.Length * 4];
        for (int i = 0; i < w.Length; i++) {
            b[4 * i] = (byte)(w[i] >> 24); b[4 * i + 1] = (byte)(w[i] >> 16);
            b[4 * i + 2] = (byte)(w[i] >> 8); b[4 * i + 3] = (byte)w[i];
        }
        return b;
    }
    public static byte[] FromBase64Url(string s) {
        s = s.Replace('-', '+').Replace('_', '/').Replace(",", "");
        while (s.Length % 4 != 0) s += "=";
        return Convert.FromBase64String(s);
    }
    // Fills the buffer unless the stream ends: blocks must stay 16-byte aligned across reads.
    static int ReadFull(Stream s, byte[] buffer) {
        int total = 0, n;
        while (total < buffer.Length && (n = s.Read(buffer, total, buffer.Length - total)) > 0) total += n;
        return total;
    }
    static Aes NewAes(byte[] key) {
        var aes = Aes.Create();
        aes.Key = key; aes.Mode = CipherMode.ECB; aes.Padding = PaddingMode.None;
        return aes;
    }
    static byte[] AesKey(uint[] k) { return Bytes(k[0] ^ k[4], k[1] ^ k[5], k[2] ^ k[6], k[3] ^ k[7]); }

    // The file's attributes ("MEGA{\"n\":\"name\"}"), AES-CBC with a zero IV.
    public static string Attributes(string at, string linkKey) {
        var k = Words(FromBase64Url(linkKey));
        var data = FromBase64Url(at);
        using (var aes = Aes.Create()) {
            aes.Key = AesKey(k); aes.Mode = CipherMode.CBC; aes.Padding = PaddingMode.None; aes.IV = new byte[16];
            var plain = aes.CreateDecryptor().TransformFinalBlock(data, 0, data.Length);
            return System.Text.Encoding.UTF8.GetString(plain).TrimEnd('\0');
        }
    }

    // Decrypts `input` into `output`; returns true when the chunk MACs match the link's meta-MAC.
    public static bool Decrypt(string input, string output, string linkKey) {
        var k = Words(FromBase64Url(linkKey));
        using (var aes = NewAes(AesKey(k)))
        using (var enc = aes.CreateEncryptor())
        using (var src = File.OpenRead(input))
        using (var dst = File.Create(output)) {
            var nonce = Bytes(k[4], k[5]);
            var macIv = Bytes(k[4], k[5], k[4], k[5]);
            var fileMac = new byte[16];
            var counter = new byte[16]; Array.Copy(nonce, counter, 8);
            var stream = new byte[16];
            long position = 0, chunkSize = 128 * 1024, next = 0;
            var buffer = new byte[1 << 20];
            var chunkMac = (byte[])macIv.Clone();
            long chunkEnd = chunkSize;
            var block = new byte[16];
            int read;
            while ((read = ReadFull(src, buffer)) > 0) {
                for (int off = 0; off < read; off += 16) {
                    // keystream block for this 16-byte offset
                    ulong index = (ulong)(position / 16);
                    for (int i = 0; i < 8; i++) counter[15 - i] = (byte)(index >> (8 * i));
                    enc.TransformBlock(counter, 0, 16, stream, 0);
                    int n = Math.Min(16, read - off);
                    Array.Clear(block, 0, 16);
                    for (int i = 0; i < n; i++) { buffer[off + i] ^= stream[i]; block[i] = buffer[off + i]; }
                    // CBC-MAC of the plaintext, per chunk
                    for (int i = 0; i < 16; i++) chunkMac[i] ^= block[i];
                    enc.TransformBlock(chunkMac, 0, 16, chunkMac, 0);
                    position += n;
                    if (position == chunkEnd) {
                        for (int i = 0; i < 16; i++) fileMac[i] ^= chunkMac[i];
                        enc.TransformBlock(fileMac, 0, 16, fileMac, 0);
                        chunkMac = (byte[])macIv.Clone();
                        if (chunkSize < 1024 * 1024) chunkSize += 128 * 1024;
                        chunkEnd += chunkSize;
                        next = position;
                    }
                }
                dst.Write(buffer, 0, read);
            }
            if (position != next) { // the last, partial chunk
                for (int i = 0; i < 16; i++) fileMac[i] ^= chunkMac[i];
                enc.TransformBlock(fileMac, 0, 16, fileMac, 0);
            }
            var m = Words(fileMac);
            return (m[0] ^ m[1]) == k[6] && (m[2] ^ m[3]) == k[7];
        }
    }
}
"@
}

function Get-P2Sha256([string]$Path) { (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant() }

# Downloads one extra into $Cache (once) and returns its path, or $null with a warning.
function Get-P2Extra([string]$Name, [string]$Cache) {
    $e = $script:P2Extras[$Name]
    New-Item -ItemType Directory -Force -Path $Cache | Out-Null
    if ($e.Mega) { return Get-P2MegaExtra $e $Cache }
    $path = Join-Path $Cache $e.File
    if (-not (Test-Path -LiteralPath $path)) {
        Write-Host "Downloading $($e.Title)..."
        $partial = "$path.partial"
        & curl.exe --fail --location --retry 3 --silent --show-error --output $partial $e.Url
        if ($LASTEXITCODE -ne 0) { Remove-Item -LiteralPath $partial -ErrorAction SilentlyContinue; Write-Warning "Could not download $($e.Url)."; return $null }
        Move-Item -Force -LiteralPath $partial -Destination $path
    }
    if (-not (Test-P2Extra $e $path)) { return $null }
    return $path
}

# Only a file with the pinned SHA-256 is used (for a new extra, the warning gives the digest to pin).
function Test-P2Extra($e, [string]$Path) {
    $sha = Get-P2Sha256 $Path
    if ($sha -eq $e.Sha256) { return $true }
    Write-Warning "$Path does not match its pinned SHA-256 (it is $sha); delete it to download it again."
    return $false
}

function Get-P2MegaExtra($e, [string]$Cache) {
    $link = $e.Mega
    if ($link -notmatch '/file/([^#]+)#(.+)$') { Write-Warning "Not a MEGA file link: $link"; return $null }
    $id = $Matches[1]; $key = $Matches[2]
    $dir = Join-Path $Cache "mega-$id"
    $done = Get-ChildItem -LiteralPath $dir -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -notlike "*.encrypted" }
    if ($done) { $path = $done[0].FullName; if (Test-P2Extra $e $path) { return $path } else { return $null } }

    $body = "[{`"a`":`"g`",`"g`":1,`"p`":`"$id`"}]"
    try { $reply = Invoke-RestMethod -Method Post -Uri "https://g.api.mega.co.nz/cs?id=1" -Body $body -ContentType "application/json" }
    catch { Write-Warning "MEGA did not answer: $($_.Exception.Message)"; return $null }
    $info = @($reply)[0]
    if ($info -is [int] -or $info -is [long] -or -not $info.g) { Write-Warning "MEGA refused the file (code $info)."; return $null }
    $attributes = [P2Mega]::Attributes($info.at, $key)
    if ($attributes -notmatch '^MEGA(\{.*\})') { Write-Warning "Could not read the MEGA file's name (wrong key?)."; return $null }
    $fileName = ($Matches[1] | ConvertFrom-Json).n
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $encrypted = Join-Path $dir "$fileName.encrypted"
    Write-Host ("Downloading {0} ({1}, {2:N0} MB) from MEGA..." -f $e.Title, $fileName, ($info.s / 1MB))
    & curl.exe --fail --location --retry 3 --silent --show-error --output $encrypted $info.g
    if ($LASTEXITCODE -ne 0) { Remove-Item -LiteralPath $encrypted -ErrorAction SilentlyContinue; Write-Warning "Could not download the MEGA file."; return $null }
    $path = Join-Path $dir $fileName
    $macOk = [P2Mega]::Decrypt($encrypted, $path, $key)
    Remove-Item -LiteralPath $encrypted
    if (-not $macOk) { Remove-Item -LiteralPath $path; Write-Warning "The MEGA file failed its integrity check."; return $null }
    if (Test-P2Extra $e $path) { return $path } else { return $null }
}
