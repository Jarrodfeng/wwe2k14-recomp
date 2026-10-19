# Session 35: a screenshot of the whole primary screen (the game window with its ImGui overlay, which the game's own
# screenshots leave out), scaled to -Width, as JPEG.
#   pwsh -NoProfile -File tools\cinecam\desktop_shot.ps1 -Out out\live\desk.jpg [-Width 1600]
param([Parameter(Mandatory)][string]$Out, [int]$Width = 1600)
Add-Type -AssemblyName System.Drawing, System.Windows.Forms
$b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
$h = [int]($b.Height * $Width / $b.Width)
$small = New-Object System.Drawing.Bitmap $bmp, $Width, $h
[void][IO.Directory]::CreateDirectory((Split-Path -Parent ([IO.Path]::GetFullPath($Out))))
$small.Save([IO.Path]::GetFullPath($Out), [System.Drawing.Imaging.ImageFormat]::Jpeg)
$g.Dispose(); $bmp.Dispose(); $small.Dispose()
"saved $Out ($($b.Width)x$($b.Height) -> ${Width}x$h)"
