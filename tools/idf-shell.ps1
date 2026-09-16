# Turns the current PowerShell into an ESP-IDF v6.1 shell (EIM install: C:\dev\esp, tools C:\Espressif).
#   . tools\idf-shell.ps1
#   idf.py -C firmware build
# PYTHONPATH is cleared first: this machine has a system-wide one (SVP 4) that breaks other Pythons.
$env:PYTHONPATH = ''
$env:PATH = ($env:PATH -split ';' | Where-Object { $_ -notlike '*SVP 4*' }) -join ';'
. 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
