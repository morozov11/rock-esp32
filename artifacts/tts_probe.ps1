Add-Type -AssemblyName System.Speech
$synth = New-Object System.Speech.Synthesis.SpeechSynthesizer
$synth.GetInstalledVoices() | ForEach-Object {
    Write-Output ($_.VoiceInfo.Name + ' | ' + $_.VoiceInfo.Culture)
}
