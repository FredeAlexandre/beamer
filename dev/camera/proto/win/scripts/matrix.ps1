# Lifetime x access matrix: create (+Start) with a short hold, check enumeration during the hold and after the
# creating process exits, then remove. Prints one line per case.
param([string]$Tag = "")
Set-Location C:\Users\Alex\camera-c2\bin
function Present { $l = .\camprobe.exe list | ConvertFrom-Json; [bool]($l.mf | ? { $_.name -like "Beamer Camera*" }) }
foreach ($life in "session", "system") {
  foreach ($acc in "user", "all") {
    $out = "mx-$life-$acc.out"
    $p = Start-Process -PassThru -NoNewWindow -FilePath .\vcamctl.exe -ArgumentList "create $life $acc --hold 4" -RedirectStandardOutput $out
    Start-Sleep 2
    $during = Present
    $p.WaitForExit()
    Start-Sleep 2
    $after = Present
    $res = (Get-Content $out | Select -First 1)
    $rm = .\vcamctl.exe remove $life $acc
    Start-Sleep 1
    $afterRm = Present
    "{0} {1}/{2}: create={3} during={4} after_exit={5} remove={6} after_remove={7}" -f $Tag, $life, $acc, $res, $during, $after, $rm, $afterRm
  }
}
