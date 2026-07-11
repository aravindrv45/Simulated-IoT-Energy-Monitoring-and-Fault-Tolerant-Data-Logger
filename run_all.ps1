$root = $PSScriptRoot

Start-Process powershell -WorkingDirectory $root `
    -ArgumentList "-NoExit", "-Command", ".\wokwi-cli.exe gateway-node"

Start-Sleep -Seconds 3

Start-Process powershell -WorkingDirectory $root `
    -ArgumentList "-NoExit", "-Command", ".\wokwi-cli.exe slave-node"

Start-Sleep -Seconds 3

Start-Process powershell -WorkingDirectory (Join-Path $root "bridge") `
    -ArgumentList "-NoExit", "-Command", "python bridge.py"