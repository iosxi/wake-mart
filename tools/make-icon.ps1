# wakemart.ico を作る（目覚まし時計の絵）。Windows PowerShell 5.1 で動かす:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\make-icon.ps1
Add-Type -AssemblyName System.Drawing
$ErrorActionPreference = 'Stop'
$out = Join-Path (Split-Path $PSScriptRoot -Parent) 'wakemart.ico'
$sizes = 16, 20, 24, 32, 40, 48, 64, 256

function Draw([int]$s) {
    $bmp = New-Object System.Drawing.Bitmap $s, $s, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.Clear([System.Drawing.Color]::Transparent)
    $k = $s / 256.0
    function Rc([double]$x, [double]$y, [double]$w, [double]$h) { New-Object System.Drawing.RectangleF ([float]($x*$k)), ([float]($y*$k)), ([float]($w*$k)), ([float]($h*$k)) }

    $body  = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 232, 93, 60))
    $dark  = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 150, 45, 30))
    $face  = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 255, 250, 240))
    $hand  = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 50, 40, 40)), ([float][Math]::Max(1.2, 16*$k))
    $hand.StartCap = 'Round'; $hand.EndCap = 'Round'
    $legs  = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 150, 45, 30)), ([float][Math]::Max(1.2, 18*$k))
    $legs.StartCap = 'Round'; $legs.EndCap = 'Round'

    # 脚
    $g.DrawLine($legs, [float](70*$k), [float](200*$k), [float](44*$k), [float](236*$k))
    $g.DrawLine($legs, [float](186*$k), [float](200*$k), [float](212*$k), [float](236*$k))
    # ベル
    $g.FillEllipse($dark, (Rc 14 14 92 92))
    $g.FillEllipse($dark, (Rc 150 14 92 92))
    $g.FillEllipse($dark, (Rc 18 30 84 40))
    $g.FillEllipse($dark, (Rc 154 30 84 40))
    # 本体と文字盤
    $g.FillEllipse($body, (Rc 28 40 200 200))
    $g.FillEllipse($face, (Rc 52 64 152 152))
    # 針
    $cx = 128*$k; $cy = 140*$k
    $g.DrawLine($hand, [float]$cx, [float]$cy, [float](128*$k), [float](86*$k))
    $g.DrawLine($hand, [float]$cx, [float]$cy, [float](168*$k), [float](160*$k))
    $g.FillEllipse($dark, (Rc 118 130 20 20))

    $g.Dispose()
    return $bmp
}

$images = foreach ($s in $sizes) {
    $bmp = Draw $s
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    ,@($s, $ms.ToArray())
}

$fs = [System.IO.File]::Create($out)
$w = New-Object System.IO.BinaryWriter $fs
$w.Write([UInt16]0); $w.Write([UInt16]1); $w.Write([UInt16]$images.Count)
$offset = 6 + 16 * $images.Count
foreach ($im in $images) {
    $s = $im[0]; $data = $im[1]
    $b = if ($s -ge 256) { 0 } else { $s }
    $w.Write([byte]$b); $w.Write([byte]$b); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([UInt16]1); $w.Write([UInt16]32)
    $w.Write([UInt32]$data.Length); $w.Write([UInt32]$offset)
    $offset += $data.Length
}
foreach ($im in $images) { $w.Write([byte[]]$im[1]) }
$w.Close()
"wrote $out"
