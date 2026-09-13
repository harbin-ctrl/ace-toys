# Packs PNG images into a Windows .ico file.
#
# Windows Vista and later read PNG icon entries directly, so each image goes
# in as it is, with no re-encoding:
#
#   ICONDIR (6 bytes)  ICONDIRENTRY x N (16 bytes each)  PNG 1  PNG 2 ...
#
# Usage: make-ico.ps1 -Out toy.ico -Png "icon_32.png;icon_48.png;icon_256.png"
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [Parameter(Mandatory = $true)][string]$Png
)
$ErrorActionPreference = "Stop"

$IconDirSize = 6
$IconEntrySize = 16
$IconTypeIcon = 1
$MaxIconSide = 256
$PngWidthOffset = 16    # IHDR width, then height, both big-endian

function Read-BigEndian32([byte[]]$bytes, [int]$at) {
    return ([int]$bytes[$at] -shl 24) -bor ([int]$bytes[$at + 1] -shl 16) -bor
           ([int]$bytes[$at + 2] -shl 8) -bor [int]$bytes[$at + 3]
}

$files = @($Png.Split(";") | Where-Object { $_ })
$images = @($files | ForEach-Object { , [IO.File]::ReadAllBytes((Resolve-Path $_)) })

$stream = New-Object IO.MemoryStream
$writer = New-Object IO.BinaryWriter($stream)
$writer.Write([UInt16]0)
$writer.Write([UInt16]$IconTypeIcon)
$writer.Write([UInt16]$images.Count)

$offset = $IconDirSize + $IconEntrySize * $images.Count
for ($i = 0; $i -lt $images.Count; $i++) {
    $image = $images[$i]
    $width = Read-BigEndian32 $image $PngWidthOffset
    $height = Read-BigEndian32 $image ($PngWidthOffset + 4)
    if ($width -gt $MaxIconSide -or $height -gt $MaxIconSide) {
        throw "$($files[$i]) is ${width}x${height}; an icon entry holds at most ${MaxIconSide}x${MaxIconSide}"
    }
    # A side of 256 is stored as 0.
    $writer.Write([byte]($width % $MaxIconSide))
    $writer.Write([byte]($height % $MaxIconSide))
    $writer.Write([byte]0)          # no palette
    $writer.Write([byte]0)          # reserved
    $writer.Write([UInt16]1)        # colour planes
    $writer.Write([UInt16]32)       # bits per pixel
    $writer.Write([UInt32]$image.Length)
    $writer.Write([UInt32]$offset)
    $offset += $image.Length
}
foreach ($image in $images) {
    $writer.Write($image)
}
$writer.Flush()
[IO.File]::WriteAllBytes($Out, $stream.ToArray())
