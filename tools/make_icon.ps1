# Generates src/res/app.ico (a small whiteboard with a grid and a blue pen stroke).
# Run from the repo root:  powershell -ExecutionPolicy Bypass -File tools/make_icon.ps1
param([string]$Out = "src/res/app.ico")
Add-Type -AssemblyName System.Drawing

function New-IconPng([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'; $g.PixelOffsetMode = 'HighQuality'; $g.Clear([System.Drawing.Color]::Transparent)
    $s = $size / 256.0
    # Board: rounded square
    $inset = [math]::Max(1, 14 * $s); $r = 44 * $s
    $rect = New-Object System.Drawing.RectangleF $inset, $inset, ($size - 2 * $inset), ($size - 2 * $inset)
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = 2 * $r
    $path.AddArc($rect.X, $rect.Y, $d, $d, 180, 90); $path.AddArc($rect.Right - $d, $rect.Y, $d, $d, 270, 90)
    $path.AddArc($rect.Right - $d, $rect.Bottom - $d, $d, $d, 0, 90); $path.AddArc($rect.X, $rect.Bottom - $d, $d, $d, 90, 90)
    $path.CloseFigure()
    $g.FillPath((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 250, 250, 247))), $path)
    # Grid (only where it stays readable)
    if ($size -ge 32) {
        $g.SetClip($path)
        $gridPen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(40, 42, 47, 54)), ([math]::Max(1, 4 * $s))
        for ($v = $rect.X + 40 * $s; $v -lt $rect.Right; $v += 40 * $s) {
            $g.DrawLine($gridPen, $v, $rect.Y, $v, $rect.Bottom); $g.DrawLine($gridPen, $rect.X, $v, $rect.Right, $v)
        }
        $g.ResetClip()
    }
    $border = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 42, 47, 54)), ([math]::Max(1.2, 12 * $s))
    $g.DrawPath($border, $path)
    # Pen stroke: a smooth blue wave
    $pts = @()
    for ($i = 0; $i -le 40; $i++) {
        $t = $i / 40.0
        $pts += New-Object System.Drawing.PointF ((58 + 140 * $t) * $s), ((150 - 38 * [math]::Sin($t * 2 * [math]::PI)) * $s)
    }
    $ink = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 0, 144, 255)), ([math]::Max(1.5, 22 * $s))
    $ink.StartCap = 'Round'; $ink.EndCap = 'Round'; $ink.LineJoin = 'Round'
    $g.DrawCurve($ink, $pts)
    # Orange dot (pen tip)
    $dot = 30 * $s
    $g.FillEllipse((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 247, 107, 21))), (186 * $s), (66 * $s), $dot, $dot)
    $g.Dispose()
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    return ,$ms.ToArray()
}

$sizes = 16, 20, 24, 32, 40, 48, 64, 256
$images = foreach ($sz in $sizes) { ,(New-IconPng $sz) }
$ms = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter $ms
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$sizes.Count)  # ICONDIR
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {                                  # ICONDIRENTRY (PNG payload)
    $sz = $sizes[$i]; $len = $images[$i].Length
    $w.Write([byte]($(if ($sz -ge 256) { 0 } else { $sz }))); $w.Write([byte]($(if ($sz -ge 256) { 0 } else { $sz })))
    $w.Write([byte]0); $w.Write([byte]0); $w.Write([uint16]1); $w.Write([uint16]32)
    $w.Write([uint32]$len); $w.Write([uint32]$offset); $offset += $len
}
foreach ($img in $images) { $w.Write($img) }
$w.Flush()
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
[System.IO.File]::WriteAllBytes((Join-Path (Get-Location) $Out), $ms.ToArray())
"Wrote $Out ($($ms.Length) bytes, sizes: $($sizes -join ', '))"
