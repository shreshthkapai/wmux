function ConvertTo-WmuxPipeComponent {
  param(
    [AllowEmptyString()]
    [string]$Value
  )

  $sanitized = $Value -replace '[^A-Za-z0-9_.-]', '_'
  if ([string]::IsNullOrEmpty($sanitized)) {
    return "unknown-user"
  }

  $sanitized
}

function Get-WmuxCurrentUserPipeTag {
  try {
    $identity = [System.Security.Principal.WindowsIdentity]::GetCurrent()
    if ($identity -and $identity.User -and $identity.User.Value) {
      return ConvertTo-WmuxPipeComponent -Value $identity.User.Value
    }
  } catch {
  }

  return ConvertTo-WmuxPipeComponent -Value ([Environment]::UserName)
}

function Get-WmuxCommandPipeName {
  "wmux-$(Get-WmuxCurrentUserPipeTag)"
}

function Get-WmuxAttachPipeName {
  "$(Get-WmuxCommandPipeName)-attach"
}

if (-not (Get-Variable -Name WmuxNextIpcRequestId -Scope Script -ErrorAction SilentlyContinue)) {
  $script:WmuxNextIpcRequestId = [uint64]1
}

function New-WmuxIpcRequestId {
  $id = [uint64]$script:WmuxNextIpcRequestId
  $script:WmuxNextIpcRequestId = [uint64]($script:WmuxNextIpcRequestId + 1)
  $id
}

function Get-WmuxIpcFrameKindByte {
  param(
    [ValidateSet("Control", "AttachInput", "AttachOutput", "Event", "Error")]
    [string]$Kind
  )

  switch ($Kind) {
    "Control" { return [byte]1 }
    "AttachInput" { return [byte]2 }
    "AttachOutput" { return [byte]3 }
    "Event" { return [byte]4 }
    "Error" { return [byte]5 }
  }
}

function Get-WmuxIpcFrameKindName {
  param([byte]$Kind)

  switch ($Kind) {
    1 { return "Control" }
    2 { return "AttachInput" }
    3 { return "AttachOutput" }
    4 { return "Event" }
    5 { return "Error" }
    default { return "Unknown" }
  }
}

function New-WmuxIpcFrame {
  param(
    [ValidateSet("Control", "AttachInput", "AttachOutput", "Event", "Error")]
    [string]$Kind,

    [uint64]$RequestId,

    [byte[]]$Payload = @()
  )

  $frame = [byte[]]::new(19 + $Payload.Length)
  $frame[0] = [byte][char]"W"
  $frame[1] = [byte][char]"M"
  $frame[2] = [byte][char]"U"
  $frame[3] = [byte][char]"X"
  $frame[4] = 1
  $frame[5] = 0
  $frame[6] = Get-WmuxIpcFrameKindByte -Kind $Kind

  $requestBytes = [BitConverter]::GetBytes([uint64]$RequestId)
  [Array]::Copy($requestBytes, 0, $frame, 7, 8)
  $lengthBytes = [BitConverter]::GetBytes([uint32]$Payload.Length)
  [Array]::Copy($lengthBytes, 0, $frame, 15, 4)

  if ($Payload.Length -gt 0) {
    [Array]::Copy($Payload, 0, $frame, 19, $Payload.Length)
  }

  $frame
}

function Read-WmuxExactBytes {
  param(
    [Parameter(Mandatory = $true)]
    [System.IO.Stream]$Pipe,

    [Parameter(Mandatory = $true)]
    [int]$Count,

    [Parameter(Mandatory = $true)]
    [DateTime]$Deadline
  )

  $bytes = [byte[]]::new($Count)
  $offset = 0
  while ($offset -lt $Count) {
    $remainingBytes = $Count - $offset
    $readTask = $Pipe.ReadAsync($bytes, $offset, $remainingBytes)
    $remainingMs = [int][Math]::Max(1, ($Deadline - [DateTime]::UtcNow).TotalMilliseconds)
    if (-not $readTask.Wait($remainingMs)) {
      return $null
    }

    if ($readTask.Result -le 0) {
      return $null
    }

    $offset += $readTask.Result
  }

  $bytes
}

function Read-WmuxIpcFrame {
  param(
    [Parameter(Mandatory = $true)]
    [System.IO.Stream]$Pipe,

    [ValidateRange(1, 120)]
    [int]$TimeoutSeconds = 10,

    [ValidateRange(0, 120000)]
    [int]$TimeoutMilliseconds = 0
  )

  $deadline = if ($TimeoutMilliseconds -gt 0) {
    [DateTime]::UtcNow.AddMilliseconds($TimeoutMilliseconds)
  } else {
    [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
  }
  $header = Read-WmuxExactBytes -Pipe $Pipe -Count 19 -Deadline $deadline
  if ($null -eq $header) {
    throw "timed out waiting for IPC frame header"
  }

  $magic = [Text.Encoding]::ASCII.GetString($header, 0, 4)
  if ($magic -ne "WMUX") {
    throw "bad IPC frame magic: $magic"
  }

  $version = [BitConverter]::ToUInt16($header, 4)
  if ($version -ne 1) {
    throw "unsupported IPC frame version: $version"
  }

  $kindByte = [byte]$header[6]
  $kind = Get-WmuxIpcFrameKindName -Kind $kindByte
  if ($kind -eq "Unknown") {
    throw "unknown IPC frame kind: $kindByte"
  }

  $requestId = [BitConverter]::ToUInt64($header, 7)
  $length = [BitConverter]::ToUInt32($header, 15)
  if ($length -gt 16777216) {
    throw "IPC frame payload too large: $length"
  }

  $payload = [byte[]]::new(0)
  if ($length -gt 0) {
    $payload = Read-WmuxExactBytes -Pipe $Pipe -Count ([int]$length) -Deadline $deadline
    if ($null -eq $payload) {
      throw "timed out waiting for IPC frame payload"
    }
  }

  [pscustomobject]@{
    Kind = $kind
    RequestId = $requestId
    Payload = $payload
  }
}

function Write-WmuxIpcFrame {
  param(
    [Parameter(Mandatory = $true)]
    [System.IO.Stream]$Pipe,

    [ValidateSet("Control", "AttachInput", "AttachOutput", "Event", "Error")]
    [string]$Kind,

    [uint64]$RequestId,

    [byte[]]$Payload = @()
  )

  $frame = New-WmuxIpcFrame -Kind $Kind -RequestId $RequestId -Payload $Payload
  $Pipe.Write($frame, 0, $frame.Length)
  $Pipe.Flush()
}

function Write-WmuxAttachPayloadFrame {
  param(
    [Parameter(Mandatory = $true)]
    [System.IO.Stream]$Pipe,

    [Parameter(Mandatory = $true)]
    [byte[]]$Payload
  )

  Write-WmuxIpcFrame `
    -Pipe $Pipe `
    -Kind AttachInput `
    -RequestId (New-WmuxIpcRequestId) `
    -Payload $Payload
}

function Open-WmuxAttachPipe {
  param(
    [ValidateRange(1, 120)]
    [int]$TimeoutSeconds = 10
  )

  $pipe = [System.IO.Pipes.NamedPipeClientStream]::new(
    ".",
    (Get-WmuxAttachPipeName),
    [System.IO.Pipes.PipeDirection]::InOut,
    [System.IO.Pipes.PipeOptions]::Asynchronous)

  $pipe.Connect($TimeoutSeconds * 1000)
  $pipe
}

function Start-WmuxAttach {
  param(
    [Parameter(Mandatory = $true)]
    [System.IO.Stream]$Pipe,

    [Parameter(Mandatory = $true)]
    [string]$SessionName,

    [int]$Columns = 120,

    [int]$Rows = 30,

    [ValidateRange(1, 120)]
    [int]$TimeoutSeconds = 10
  )

  $requestId = New-WmuxIpcRequestId
  $request = '{{"type":"AttachStart","session_name":"{0}","terminal_columns":{1},"terminal_rows":{2}}}' -f $SessionName, $Columns, $Rows
  Write-WmuxIpcFrame `
    -Pipe $Pipe `
    -Kind Control `
    -RequestId $requestId `
    -Payload ([Text.Encoding]::UTF8.GetBytes($request + "`n"))

  $frame = Read-WmuxIpcFrame -Pipe $Pipe -TimeoutSeconds $TimeoutSeconds
  if ($frame.RequestId -ne $requestId) {
    throw "attach response request id mismatch"
  }
  if ($frame.Kind -ne "Control" -and $frame.Kind -ne "Error") {
    throw "unexpected attach response frame kind: $($frame.Kind)"
  }

  $response = [Text.Encoding]::UTF8.GetString($frame.Payload) | ConvertFrom-Json
  if (-not $response.ok) {
    throw "attach failed: $($response.message)"
  }

  $response
}

function Read-WmuxAttachOutputText {
  param(
    [Parameter(Mandatory = $true)]
    [System.IO.Stream]$Pipe,

    [ValidateRange(1, 120)]
    [int]$TimeoutSeconds = 10,

    [ValidateRange(0, 120000)]
    [int]$TimeoutMilliseconds = 0
  )

  while ($true) {
    $frame = Read-WmuxIpcFrame `
      -Pipe $Pipe `
      -TimeoutSeconds $TimeoutSeconds `
      -TimeoutMilliseconds $TimeoutMilliseconds
    if ($frame.Kind -eq "AttachOutput") {
      return [Text.Encoding]::UTF8.GetString($frame.Payload)
    }

    if ($frame.Kind -eq "Error") {
      $response = [Text.Encoding]::UTF8.GetString($frame.Payload) | ConvertFrom-Json
      throw "attach stream error: $($response.message)"
    }

    throw "unexpected attach stream frame kind: $($frame.Kind)"
  }
}

function Write-WmuxScriptPipeBytes {
  param(
    [Parameter(Mandatory = $true)]
    [System.IO.Stream]$Pipe,

    [Parameter(Mandatory = $true)]
    [byte[]]$Bytes
  )

  if ($Bytes.Length -gt 0 -and $Bytes[0] -eq [byte][char]"{") {
    $requestId = New-WmuxIpcRequestId
    $script:WmuxLastAttachStartRequestId = $requestId
    Write-WmuxIpcFrame -Pipe $Pipe -Kind Control -RequestId $requestId -Payload $Bytes
    return
  }

  Write-WmuxAttachPayloadFrame -Pipe $Pipe -Payload $Bytes
}

function Read-WmuxScriptResponseLine {
  param(
    [Parameter(Mandatory = $true)]
    [System.IO.Stream]$Pipe,

    [ValidateRange(1, 120)]
    [int]$TimeoutSeconds = 10
  )

  $frame = Read-WmuxIpcFrame -Pipe $Pipe -TimeoutSeconds $TimeoutSeconds
  if ($frame.Kind -ne "Control" -and $frame.Kind -ne "Error") {
    throw "unexpected attach response frame kind: $($frame.Kind)"
  }
  if ((Get-Variable -Name WmuxLastAttachStartRequestId -Scope Script -ErrorAction SilentlyContinue) -and
      $script:WmuxLastAttachStartRequestId -ne 0 -and
      $frame.RequestId -ne $script:WmuxLastAttachStartRequestId) {
    throw "attach response request id mismatch"
  }

  [Text.Encoding]::UTF8.GetString($frame.Payload)
}
