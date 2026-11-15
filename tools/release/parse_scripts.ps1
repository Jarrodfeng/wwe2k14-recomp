# Session 102 W1/W2: the release scripts must at least parse (they are never run by the gate). Exit 0 = no parse error in any of them.
# A planted file with a syntax error (-Plant) must make this exit 1.
param([switch]$Plant)
$here = $PSScriptRoot
$files = @('make_release.ps1', 'release.ps1', 'pgo_build.ps1', 'prove_build_flags.ps1', 'release_configure_args.ps1') | ForEach-Object { Join-Path $here $_ }
if ($Plant) {
    $bad = Join-Path ([System.IO.Path]::GetTempPath()) 'parse_scripts_planted.ps1'
    Set-Content -LiteralPath $bad -Value 'if ($x -eq 1 { "missing paren" }'
    $files = @($bad)
}
$rc = 0
foreach ($f in $files) {
    if (-not (Test-Path -LiteralPath $f)) { "MISSING $f"; $rc = 1; continue }
    $errs = $null
    [void][System.Management.Automation.Language.Parser]::ParseFile($f, [ref]$null, [ref]$errs)
    if ($errs.Count) { "PARSE ERROR $(Split-Path $f -Leaf): $($errs[0].Message)"; $rc = 1 } else { "ok $(Split-Path $f -Leaf)" }
}
exit $rc
