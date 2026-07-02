[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)]
  [string]$RunDirectory,

  [ValidateRange(10, 5000)]
  [int]$MaxLogMatches = 300,

  [string]$SummaryPath = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Convert-ToMiBText {
  param([object]$Value)

  $bytes = 0.0
  if ($null -ne $Value -and [double]::TryParse([string]$Value, [ref]$bytes)) {
    return ("{0:N2} MiB" -f ($bytes / 1MB))
  }

  "n/a"
}

function Convert-ToNumber {
  param([object]$Value)

  $number = 0.0
  if ($null -ne $Value -and [double]::TryParse([string]$Value, [ref]$number)) {
    return $number
  }

  0.0
}

function Add-Line {
  param(
    [System.Collections.Generic.List[string]]$Lines,

    [string]$Text = ""
  )

  $Lines.Add($Text)
}

$resolvedRunDirectory = (Resolve-Path -LiteralPath $RunDirectory).Path
if ([string]::IsNullOrWhiteSpace($SummaryPath)) {
  $SummaryPath = Join-Path $resolvedRunDirectory "important-soak-summary.txt"
}

$samplesPath = Join-Path $resolvedRunDirectory "resource-samples.csv"
$cycleLogs = @(Get-ChildItem -LiteralPath $resolvedRunDirectory -Filter "cycle-*.log" -File -ErrorAction SilentlyContinue |
  Sort-Object Name)
$lines = [System.Collections.Generic.List[string]]::new()

Add-Line $lines "wmux soak important summary"
Add-Line $lines "generated_utc: $([DateTime]::UtcNow.ToString("o"))"
Add-Line $lines "run_directory: $resolvedRunDirectory"
Add-Line $lines "cycle_log_count: $($cycleLogs.Count)"
Add-Line $lines ""

if (Test-Path -LiteralPath $samplesPath) {
  $samples = @(Import-Csv -LiteralPath $samplesPath)
  Add-Line $lines "resource_samples: $($samples.Count)"

  if ($samples.Count -gt 0) {
    $first = $samples[0]
    $last = $samples[-1]
    $maxWorkingSet = ($samples | Measure-Object -Property working_set_bytes -Maximum).Maximum
    $maxPrivate = ($samples | Measure-Object -Property private_memory_bytes -Maximum).Maximum
    $maxHandles = ($samples | Measure-Object -Property handle_count -Maximum).Maximum
    $maxThreads = ($samples | Measure-Object -Property thread_count -Maximum).Maximum
    $maxChildren = ($samples | Measure-Object -Property child_processes -Maximum).Maximum
    $maxDropped = ($samples | Measure-Object -Property output_bytes_dropped -Maximum).Maximum
    $workingSetDelta = Convert-ToNumber $last.working_set_bytes
    $workingSetDelta -= Convert-ToNumber $first.working_set_bytes
    $privateDelta = Convert-ToNumber $last.private_memory_bytes
    $privateDelta -= Convert-ToNumber $first.private_memory_bytes
    $handleDelta = Convert-ToNumber $last.handle_count
    $handleDelta -= Convert-ToNumber $first.handle_count

    Add-Line $lines ""
    Add-Line $lines "first_sample: cycle=$($first.cycle) phase=$($first.phase) utc=$($first.timestamp_utc) ws=$(Convert-ToMiBText $first.working_set_bytes) private=$(Convert-ToMiBText $first.private_memory_bytes) handles=$($first.handle_count) children=$($first.child_processes)"
    Add-Line $lines "last_sample:  cycle=$($last.cycle) phase=$($last.phase) utc=$($last.timestamp_utc) ws=$(Convert-ToMiBText $last.working_set_bytes) private=$(Convert-ToMiBText $last.private_memory_bytes) handles=$($last.handle_count) children=$($last.child_processes)"
    Add-Line $lines "max_working_set: $(Convert-ToMiBText $maxWorkingSet)"
    Add-Line $lines "max_private_memory: $(Convert-ToMiBText $maxPrivate)"
    Add-Line $lines "max_handle_count: $maxHandles"
    Add-Line $lines "max_thread_count: $maxThreads"
    Add-Line $lines "max_child_processes: $maxChildren"
    Add-Line $lines "max_output_bytes_dropped: $maxDropped"
    Add-Line $lines "delta_working_set_first_to_last: $(Convert-ToMiBText $workingSetDelta)"
    Add-Line $lines "delta_private_first_to_last: $(Convert-ToMiBText $privateDelta)"
    Add-Line $lines "delta_handles_first_to_last: $handleDelta"
    Add-Line $lines ""
    Add-Line $lines "last_10_resource_samples:"
    Add-Line $lines (($samples |
      Select-Object -Last 10 timestamp_utc,cycle,phase,working_set_bytes,private_memory_bytes,handle_count,thread_count,child_processes,sessions,live_shells,attach_clients,render_frames,output_bytes_dropped |
      Format-Table -AutoSize | Out-String).TrimEnd())
  }
} else {
  Add-Line $lines "resource_samples: missing ($samplesPath)"
}

Add-Line $lines ""
Add-Line $lines "flagged_log_lines:"
Add-Line $lines "patterns: failed, error, exception, timeout, orphan, leak, panic, crash, assert, fatal, denied"

if ($cycleLogs.Count -eq 0) {
  Add-Line $lines "none: no cycle logs found"
} else {
  $pattern = "failed|error|exception|timeout|orphan|leak|panic|crash|assert|fatal|denied"
  $matches = @($cycleLogs |
    Select-String -Pattern $pattern -CaseSensitive:$false -ErrorAction SilentlyContinue |
    Select-Object -First $MaxLogMatches)

  if ($matches.Count -eq 0) {
    Add-Line $lines "none"
  } else {
    foreach ($match in $matches) {
      Add-Line $lines ("{0}:{1}: {2}" -f $match.Path, $match.LineNumber, $match.Line.TrimEnd())
    }

    $allMatchCount = @($cycleLogs |
      Select-String -Pattern $pattern -CaseSensitive:$false -ErrorAction SilentlyContinue).Count
    if ($allMatchCount -gt $matches.Count) {
      Add-Line $lines "... truncated flagged log lines: showing $($matches.Count) of $allMatchCount"
    }
  }
}

Add-Line $lines ""
Add-Line $lines "cycle_log_tails:"
foreach ($log in ($cycleLogs | Select-Object -Last 5)) {
  Add-Line $lines ""
  Add-Line $lines "== $($log.Name) last 30 lines =="
  $tail = @(Get-Content -LiteralPath $log.FullName -Tail 30 -ErrorAction SilentlyContinue)
  if ($tail.Count -eq 0) {
    Add-Line $lines "(empty)"
  } else {
    foreach ($line in $tail) {
      Add-Line $lines $line
    }
  }
}

$lines | Set-Content -LiteralPath $SummaryPath -Encoding UTF8
Write-Host "important soak summary: $SummaryPath"
