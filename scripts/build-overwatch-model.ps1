param(
    [ValidateSet('ow_m_320','ow_m_416','ow_m_640','ow_n_640')][string]$Name='ow_m_640',
    [string]$CalibrationDirectory,
    [string]$OutputDirectory
)
$ErrorActionPreference='Stop'
$project=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$legacy=Join-Path (Split-Path $project -Parent) 'Current/VisionForge_v17_8_47_ui_runtime_observability_hardened/android_inference_benchmark'
$sdk=Join-Path $legacy 'qnn_sdk/qairt/2.37.1.250807'
$python=Join-Path $legacy 'toolchains/python310/python.exe'
$model=Join-Path $project "models/overwatch/$Name"
$size=($Name -split '_')[-1]
if (-not $OutputDirectory) { $OutputDirectory=$model }
if (-not $CalibrationDirectory) { $CalibrationDirectory=Join-Path $model 'calibration_nhwc' }
$OutputDirectory=[System.IO.Path]::GetFullPath($OutputDirectory)
$CalibrationDirectory=(Resolve-Path -LiteralPath $CalibrationDirectory).Path
$generated=Join-Path $OutputDirectory 'qnn'
New-Item -ItemType Directory -Force $generated | Out-Null
. (Join-Path $sdk 'bin/envsetup.ps1') -arch X86_64
$env:PYTHONPATH="$(Join-Path $PSScriptRoot 'qnn_sdk_compat');$env:PYTHONPATH"
$env:ANDROID_HOME=Join-Path (Split-Path $project -Parent) '.android-sdk'
$cpp=Join-Path $generated "$($Name)_w8a16.cpp"
& $python (Join-Path $sdk 'bin/x86_64-windows-msvc/qnn-onnx-converter') `
    --input_network (Join-Path $model 'split.onnx') --output_path $cpp `
    --input_dim images "1,3,$size,$size" --input_layout images NCHW `
    --input_list (Join-Path $CalibrationDirectory 'input_list.txt') `
    --weights_bitwidth 8 --act_bitwidth 16 --bias_bitwidth 32 `
    --act_quantizer_calibration min-max --act_quantizer_schema asymmetric `
    --param_quantizer_calibration min-max --param_quantizer_schema asymmetric `
    --use_per_channel_quantization `
    --preserve_io datatype output_coordinates output_confidences
if ($LASTEXITCODE -ne 0) { throw 'QNN conversion failed' }
& (Join-Path $PSScriptRoot 'build-qnn-library.ps1') -QnnSdkRoot $sdk `
    -ModelCpp $cpp -ModelBin (Join-Path $generated "$($Name)_w8a16.bin") `
    -OutputDirectory (Join-Path $OutputDirectory 'android') -ModelName "$($Name)_w8a16"
if ($LASTEXITCODE -ne 0) { throw 'Android model library build failed' }
