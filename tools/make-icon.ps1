# Builds icon.jpg (256x256, the NRO icon) from NuvioTV's GPL-licensed logo mark
# with an "NX" badge next to the play mark.
param(
    [string]$Logo = "$PSScriptRoot\..\..\ref\NuvioTV\assets\brand\app_logo_mark.png",
    [string]$Out = "$PSScriptRoot\..\icon.jpg"
)
Add-Type -AssemblyName System.Drawing

$src = [System.Drawing.Image]::FromFile((Resolve-Path $Logo))
$size = 256
$bmp = New-Object System.Drawing.Bitmap($size, $size)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.SmoothingMode = 'AntiAlias'
$g.InterpolationMode = 'HighQualityBicubic'
$g.TextRenderingHint = 'AntiAliasGridFit'

# Background: the logo's own dark navy
$bg = [System.Drawing.Color]::FromArgb(255, 13, 16, 32)
$g.Clear($bg)

# Crop around the mark (source is 1080x1080, mark spans ~310..800 x 270..810)
$crop = New-Object System.Drawing.Rectangle(250, 230, 600, 600)
$dst = New-Object System.Drawing.Rectangle(0, 0, $size, $size)
$g.DrawImage($src, $dst, $crop, [System.Drawing.GraphicsUnit]::Pixel)

# "NX" badge, bottom-right, tucked against the play mark's right edge
$bw = 92; $bh = 50; $bx = $size - $bw - 12; $by = $size - $bh - 14; $r = 25
$path = New-Object System.Drawing.Drawing2D.GraphicsPath
$path.AddArc($bx, $by, 2 * $r, 2 * $r, 90, 180)
$path.AddArc($bx + $bw - 2 * $r, $by, 2 * $r, 2 * $r, 270, 180)
$path.CloseFigure()
$c1 = [System.Drawing.Color]::FromArgb(255, 72, 214, 240)    # cyan (logo top)
$c2 = [System.Drawing.Color]::FromArgb(255, 176, 76, 240)    # purple (logo bottom)
$grad = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
    (New-Object System.Drawing.Point($bx, $by)), (New-Object System.Drawing.Point(($bx + $bw), ($by + $bh))), $c1, $c2)
# dark outline so the badge separates from the logo
$outline = New-Object System.Drawing.Pen($bg, 5)
$g.DrawPath($outline, $path)
$g.FillPath($grad, $path)

$font = New-Object System.Drawing.Font('Segoe UI', 25, [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
$fmt = New-Object System.Drawing.StringFormat
$fmt.Alignment = 'Center'; $fmt.LineAlignment = 'Center'
$rect = New-Object System.Drawing.RectangleF($bx, ($by + 1), $bw, $bh)
$g.DrawString('NX', $font, [System.Drawing.Brushes]::White, $rect, $fmt)

# Save as high-quality JPEG (nacptool/elf2nro expect a 256x256 JPEG)
$codec = [System.Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | Where-Object { $_.MimeType -eq 'image/jpeg' }
$ep = New-Object System.Drawing.Imaging.EncoderParameters(1)
$ep.Param[0] = New-Object System.Drawing.Imaging.EncoderParameter([System.Drawing.Imaging.Encoder]::Quality, [long]95)
$outPath = [IO.Path]::GetFullPath($Out)
$bmp.Save($outPath, $codec, $ep)

$g.Dispose(); $bmp.Dispose(); $src.Dispose()
Write-Host "Wrote $outPath"
