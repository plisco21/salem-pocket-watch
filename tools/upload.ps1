# Carica salem sull'orologio collegato (trova da solo la porta USB della SuperMini).
#   .\upload.ps1 test     -> versione di test: autotest di 5 cicli, poi gioco normale
#   .\upload.ps1 pulita   -> versione normale (senza autotest)
#   aggiungere -pinScambiati per l'orologio con encoder A e pulsante invertiti (A=GPIO10, SW=GPIO20)
# Se il caricamento non riesce: tenere premuto BOOT, premere RST, rilasciare BOOT, e rilanciare.
param([ValidateSet('test', 'pulita')][string]$versione = 'pulita', [switch]$pinScambiati)

$cli  = 'C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe'
$fqbn = 'esp32:esp32:esp32c3:CDCOnBoot=cdc,PartitionScheme=huge_app'
$libs = "$env:USERPROFILE\Documents\Arduino\libraries"
$build = Join-Path $env:TEMP "salem_build_$versione"
$sketch = Join-Path $PSScriptRoot '..\firmware\salem'

$flags = @()
if ($versione -eq 'test') { $flags += '-DSALEM_AUTOTEST' }
if ($pinScambiati) { $flags += '-DSALEM_PIN_SCAMBIATI'; $build += '_scambiati' }
$extra = @()
if ($flags) { $extra = @('--build-property', "compiler.cpp.extra_flags=$($flags -join ' ')") }

Write-Host "Compilo la versione $versione..."
& $cli compile --fqbn $fqbn --libraries $libs --build-path $build @extra $sketch
if ($LASTEXITCODE -ne 0) { Write-Host 'ERRORE di compilazione'; exit 1 }

$porta = Get-CimInstance Win32_PnPEntity |
  Where-Object { $_.DeviceID -match 'VID_303A&PID_1001&MI_00' -and $_.Name -match '\((COM\d+)\)' } |
  ForEach-Object { $matches[1] } | Select-Object -First 1
if (-not $porta) { Write-Host 'Nessun orologio collegato.'; exit 1 }

Write-Host "Carico su $porta..."
& $cli upload -p $porta --fqbn $fqbn --input-dir $build $sketch
if ($LASTEXITCODE -ne 0) { Write-Host 'Caricamento non riuscito: BOOT premuto + RST, poi rilancia.'; exit 1 }
Write-Host "Fatto: versione $versione su $porta"
