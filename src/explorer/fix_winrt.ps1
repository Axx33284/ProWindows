# Run by build_explorer.bat on freshly generated C++/WinRT headers.
#
# The Windows App Runtime has a namespace Microsoft.Windows.*, and the cppwinrt
# the SDK ships (2.0.190620) writes "Windows::Foundation::X" unqualified inside
# namespace winrt::Microsoft::..., where it then finds winrt::Microsoft::Windows
# instead of winrt::Windows. Qualify those so the lookup lands where it was meant.
param([Parameter(Mandatory = $true)][string]$Dir)

$re = [regex]'(?<![\w:])Windows::'
$utf8 = New-Object System.Text.UTF8Encoding($false)
foreach ($f in [IO.Directory]::GetFiles($Dir, '*.h', [IO.SearchOption]::AllDirectories)) {
    $t = [IO.File]::ReadAllText($f)
    $n = $re.Replace($t, 'winrt::Windows::')
    if ($n -ne $t) { [IO.File]::WriteAllText($f, $n, $utf8) }
}
