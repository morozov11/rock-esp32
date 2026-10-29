Add-Type -AssemblyName System.Speech
$synth = New-Object System.Speech.Synthesis.SpeechSynthesizer
$synth.SelectVoice('Microsoft Irina Desktop')
$synth.SetOutputToWaveFile("$PWD\artifacts\tts_rock_only.wav")
$synth.Speak('рок')
$synth.Dispose()
Write-Output done
