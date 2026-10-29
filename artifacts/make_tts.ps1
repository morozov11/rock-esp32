param([string]$OutDir = "artifacts\tts")
Add-Type -AssemblyName System.Speech
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$synth = New-Object System.Speech.Synthesis.SpeechSynthesizer
$synth.SelectVoice('Microsoft Irina Desktop')
$synth.Rate = 0
$synth.SetOutputToWaveFile("$PWD\$OutDir\rock.wav")
$synth.Speak('Роксервер рок')
$synth.SetOutputToWaveFile("$PWD\$OutDir\start.wav")
$synth.Speak('Роксервер старт')
$synth.SetOutputToWaveFile("$PWD\$OutDir\next.wav")
$synth.Speak('Роксервер дальше')
$synth.SetOutputToWaveFile("$PWD\$OutDir\like.wav")
$synth.Speak('Роксервер лайк')
$synth.SetOutputToWaveFile("$PWD\$OutDir\stop.wav")
$synth.Speak('Роксервер стоп')

$synth.SetOutputToWaveFile("$PWD\$OutDir\quieter.wav")
$synth.Speak('Роксервер тише')
$synth.SetOutputToWaveFile("$PWD\$OutDir\louder.wav")
$synth.Speak('Роксервер громче')
$synth.Dispose()
Get-ChildItem $OutDir | ForEach-Object { Write-Output ($_.Name + ' ' + $_.Length) }
