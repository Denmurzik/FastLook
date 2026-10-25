#requires -Version 7
<#
.SYNOPSIS
  Sends one framed JSON message to a FastLook pipe and prints the framed reply.
.DESCRIPTION
  Connects with NamedPipeClientStream, writes <4-byte little-endian length><UTF-8 JSON>, reads one framed reply and prints
  its JSON. Exit code 0 with the reply printed; 1 when there is no server (connect timeout), 2 when no reply arrives within
  -TimeoutMs (nothing is printed to stdout).
.EXAMPLE
  pwsh scripts/ipc-send.ps1 -Pipe '\\.\pipe\FastLook-test' -Json '{"v":1,"id":1,"type":"ping"}'
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Pipe,
    [Parameter(Mandatory)][string]$Json,
    [int]$TimeoutMs = 2000
)

$ErrorActionPreference = 'Stop'

# \\.\pipe\name -> name
$name = $Pipe -replace '^\\\\\.\\pipe\\', ''
$client = [System.IO.Pipes.NamedPipeClientStream]::new('.', $name, [System.IO.Pipes.PipeDirection]::InOut, [System.IO.Pipes.PipeOptions]::Asynchronous)

function Read-Exact([System.IO.Stream]$stream, [int]$count, [System.Threading.CancellationToken]$token) {
    $buffer = [byte[]]::new($count)
    $offset = 0
    while ($offset -lt $count) {
        $n = $stream.ReadAsync($buffer, $offset, $count - $offset, $token).GetAwaiter().GetResult()
        if ($n -le 0) { throw [System.IO.EndOfStreamException]::new('Pipe closed') }
        $offset += $n
    }
    ,$buffer
}

try {
    try { $client.Connect($TimeoutMs) } catch [System.TimeoutException] { Write-Error "No server on $Pipe" -ErrorAction Continue; exit 1 }

    $payload = [System.Text.Encoding]::UTF8.GetBytes($Json)
    $frame = [byte[]]::new(4 + $payload.Length)
    [BitConverter]::GetBytes([uint32]$payload.Length).CopyTo($frame, 0)
    $payload.CopyTo($frame, 4)
    $client.Write($frame, 0, $frame.Length)
    $client.Flush()

    $cts = [System.Threading.CancellationTokenSource]::new($TimeoutMs)
    try {
        $header = Read-Exact $client 4 $cts.Token
        $length = [BitConverter]::ToUInt32($header, 0)
        $body = Read-Exact $client ([int]$length) $cts.Token
        [System.Text.Encoding]::UTF8.GetString($body)
    }
    catch [System.OperationCanceledException] {
        Write-Error "No reply within $TimeoutMs ms" -ErrorAction Continue
        exit 2
    }
    catch [System.IO.EndOfStreamException] {
        Write-Error 'Pipe closed before a reply arrived' -ErrorAction Continue
        exit 2
    }
}
finally {
    $client.Dispose()
}
