param(
    # Managed product executable to verify.
    # 要验证的托管产品可执行文件。
    [string]$Executable = (Join-Path $PSScriptRoot '..\build\c\vulcan-codebase-memory-mcp.exe'),

    # Maximum wait for one MCP response.
    # 单次 MCP 响应的最长等待时间。
    [int]$ResponseTimeoutMs = 30000
)

$ErrorActionPreference = 'Stop'
$Contract = 'vulcan.codebase-memory/1'

<#
.SYNOPSIS
Returns exact managed daemon processes for the selected executable.
返回所选可执行文件对应的精确托管 daemon 进程。
#>
function Get-ManagedDaemonProcess {
    param(
        # Canonical executable path.
        # 规范化可执行文件路径。
        [Parameter(Mandatory)]
        [string]$Path
    )

    $CanonicalExecutable = [System.IO.Path]::GetFullPath($Path)
    return @(Get-CimInstance Win32_Process | Where-Object {
            $_.ExecutablePath -and
            [System.IO.Path]::GetFullPath($_.ExecutablePath) -eq $CanonicalExecutable -and
            $_.CommandLine -like '*--cbm-vulcan-managed-internal*'
        })
}

<#
.SYNOPSIS
Reads and parses one bounded JSON-RPC response line.
读取并解析一行有界 JSON-RPC 响应。
#>
function Read-McpResponse {
    param(
        # Running MCP frontend process.
        # 正在运行的 MCP 前端进程。
        [Parameter(Mandatory)]
        [System.Diagnostics.Process]$Process,

        # Response timeout in milliseconds.
        # 响应超时时间，单位为毫秒。
        [Parameter(Mandatory)]
        [int]$TimeoutMs
    )

    $ReadTask = $Process.StandardOutput.ReadLineAsync()
    if (-not $ReadTask.Wait($TimeoutMs)) {
        throw "Timed out waiting for an MCP response after ${TimeoutMs} ms."
    }
    $Line = $ReadTask.Result
    if ([string]::IsNullOrWhiteSpace($Line)) {
        $ExitDetail = if ($Process.HasExited) {
            " exit=$($Process.ExitCode) stderr=$($Process.StandardError.ReadToEnd())"
        }
        else {
            ''
        }
        throw "The MCP frontend closed stdout before returning a response.$ExitDetail"
    }
    return $Line | ConvertFrom-Json -Depth 100
}

<#
.SYNOPSIS
Sends one JSON-RPC request and returns its parsed response.
发送一个 JSON-RPC 请求并返回解析后的响应。
#>
function Invoke-McpRequest {
    param(
        # Running MCP frontend process.
        # 正在运行的 MCP 前端进程。
        [Parameter(Mandatory)]
        [System.Diagnostics.Process]$Process,

        # JSON-RPC request object.
        # JSON-RPC 请求对象。
        [Parameter(Mandatory)]
        [hashtable]$Request,

        # Response timeout in milliseconds.
        # 响应超时时间，单位为毫秒。
        [Parameter(Mandatory)]
        [int]$TimeoutMs
    )

    $Payload = $Request | ConvertTo-Json -Compress -Depth 100
    $Process.StandardInput.WriteLine($Payload)
    $Process.StandardInput.Flush()
    return Read-McpResponse -Process $Process -TimeoutMs $TimeoutMs
}

<#
.SYNOPSIS
Throws when an end-to-end invariant is false.
端到端不变量为 false 时抛出异常。
#>
function Assert-E2e {
    param(
        # Invariant result.
        # 不变量结果。
        [Parameter(Mandatory)]
        [bool]$Condition,

        # Failure description.
        # 失败说明。
        [Parameter(Mandatory)]
        [string]$Message
    )

    if (-not $Condition) {
        throw $Message
    }
}

<#
.SYNOPSIS
Removes only the exact test-owned temporary tree.
仅删除测试拥有的精确临时目录树。
#>
function Remove-TestTree {
    param(
        # Test-owned directory path.
        # 测试拥有的目录路径。
        [string]$Path
    )

    if ([string]::IsNullOrWhiteSpace($Path) -or -not (Test-Path -LiteralPath $Path)) {
        return
    }
    $CanonicalPath = [System.IO.Path]::GetFullPath($Path)
    $CanonicalBase =
        [System.IO.Path]::GetFullPath([Environment]::GetFolderPath('LocalApplicationData'))
    $Leaf = Split-Path -Leaf $CanonicalPath
    if (-not $CanonicalPath.StartsWith($CanonicalBase, [System.StringComparison]::OrdinalIgnoreCase) -or
        -not $Leaf.StartsWith('cbm-vulcan-e2e-', [System.StringComparison]::Ordinal)) {
        throw "Refusing to remove a non-test path: $CanonicalPath"
    }
    Remove-Item -LiteralPath $CanonicalPath -Recurse -Force
}

$CanonicalExecutable = [System.IO.Path]::GetFullPath($Executable)
Assert-E2e -Condition (Test-Path -LiteralPath $CanonicalExecutable -PathType Leaf) `
    -Message "Managed executable does not exist: $CanonicalExecutable"

$ExistingDaemons = Get-ManagedDaemonProcess -Path $CanonicalExecutable
Assert-E2e -Condition ($ExistingDaemons.Count -eq 0) `
    -Message 'A managed daemon already exists; refusing to reuse or terminate a non-test process.'

$TestRoot = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) `
    ("cbm-vulcan-e2e-" + [guid]::NewGuid().ToString('N'))
$CacheRoot = Join-Path $TestRoot 'cache'
$ProjectRoots = @(
    (Join-Path $TestRoot 'project-one'),
    (Join-Path $TestRoot 'project-two'),
    (Join-Path $TestRoot 'project-three')
)
$Process = $null
$CreatedDaemonIds = @()

try {
    [void](New-Item -ItemType Directory -Path $CacheRoot -Force)
    foreach ($ProjectRoot in $ProjectRoots) {
        [void](New-Item -ItemType Directory -Path $ProjectRoot -Force)
        Set-Content -LiteralPath (Join-Path $ProjectRoot 'main.c') -Value 'int main(void) { return 0; }' `
            -Encoding utf8NoBOM
    }

    $Start = [System.Diagnostics.ProcessStartInfo]::new()
    $Start.FileName = $CanonicalExecutable
    $Start.ArgumentList.Add('--vulcan-managed')
    $Start.WorkingDirectory = $ProjectRoots[0]
    $Start.UseShellExecute = $false
    $Start.RedirectStandardInput = $true
    $Start.RedirectStandardOutput = $true
    $Start.RedirectStandardError = $true
    $Start.CreateNoWindow = $true
    # Match Vulcan Code's isolated child environment instead of inheriting the caller environment.
    # 匹配 Vulcan Code 的隔离子进程环境，而不是继承调用方环境。
    $Start.Environment.Clear()
    $Start.Environment['VULCAN_CBM_CACHE_DIR'] = $CacheRoot
    $Process = [System.Diagnostics.Process]::new()
    $Process.StartInfo = $Start
    Assert-E2e -Condition $Process.Start() -Message 'Unable to start the managed MCP frontend.'

    $Initialize = Invoke-McpRequest -Process $Process -TimeoutMs $ResponseTimeoutMs -Request @{
        jsonrpc = '2.0'
        id = 1
        method = 'initialize'
        params = @{
            protocolVersion = '2025-03-26'
            capabilities = @{}
            clientInfo = @{ name = 'vulcan-managed-e2e'; version = '1' }
        }
    }
    Assert-E2e -Condition ($Initialize.result.serverInfo.name -eq 'vulcan-codebase-memory-mcp') `
        -Message 'Managed initialize response returned the wrong product identity.'
    Assert-E2e -Condition ($Initialize.result.serverInfo.contract -eq $Contract) `
        -Message 'Managed initialize response returned the wrong contract.'

    $Tools = Invoke-McpRequest -Process $Process -TimeoutMs $ResponseTimeoutMs -Request @{
        jsonrpc = '2.0'
        id = 2
        method = 'tools/list'
        params = @{}
    }
    $ToolNames = @($Tools.result.tools | ForEach-Object { $_.name })
    foreach ($RequiredTool in @('vulcan_sync_projects', 'vulcan_project_status',
            'vulcan_reindex_project', 'get_graph_schema')) {
        Assert-E2e -Condition ($ToolNames -contains $RequiredTool) `
            -Message "Managed tool surface is missing $RequiredTool."
    }
    foreach ($ForbiddenTool in @('list_projects', 'delete_project', 'manage_adr', 'ingest_traces')) {
        Assert-E2e -Condition ($ToolNames -notcontains $ForbiddenTool) `
            -Message "Managed tool surface unexpectedly exposes $ForbiddenTool."
    }

    $Projects = for ($Index = 0; $Index -lt $ProjectRoots.Count; $Index++) {
        @{
            project_id = "project-$($Index + 1)"
            pwd = [System.IO.Path]::GetFullPath($ProjectRoots[$Index])
        }
    }
    $Sync = Invoke-McpRequest -Process $Process -TimeoutMs $ResponseTimeoutMs -Request @{
        jsonrpc = '2.0'
        id = 3
        method = 'tools/call'
        params = @{
            name = 'vulcan_sync_projects'
            arguments = @{
                contract = $Contract
                generation = 1
                projects = $Projects
            }
        }
    }
    $SyncBody = $Sync.result.content[0].text | ConvertFrom-Json
    Assert-E2e -Condition ($SyncBody.project_count -eq 3 -and $SyncBody.added -eq 3) `
        -Message 'Managed synchronization did not commit all three projects.'

    foreach ($Project in $Projects) {
        $Ready = $false
        for ($Attempt = 0; $Attempt -lt 200 -and -not $Ready; $Attempt++) {
            $Status = Invoke-McpRequest -Process $Process -TimeoutMs $ResponseTimeoutMs -Request @{
                jsonrpc = '2.0'
                id = 100 + $Attempt
                method = 'tools/call'
                params = @{
                    name = 'vulcan_project_status'
                    arguments = @{
                        contract = $Contract
                        project_id = $Project.project_id
                        pwd = $Project.pwd
                    }
                }
            }
            $StatusBody = $Status.result.content[0].text | ConvertFrom-Json
            if ($StatusBody.lifecycle -eq 'failed') {
                throw "Managed indexing failed for $($Project.project_id): $($StatusBody.last_error_code)"
            }
            $Ready = $StatusBody.lifecycle -eq 'ready'
            if (-not $Ready) {
                Start-Sleep -Milliseconds 50
            }
        }
        Assert-E2e -Condition $Ready -Message "Managed project did not become ready: $($Project.project_id)"

        $Query = Invoke-McpRequest -Process $Process -TimeoutMs $ResponseTimeoutMs -Request @{
            jsonrpc = '2.0'
            id = 400
            method = 'tools/call'
            params = @{
                name = 'get_graph_schema'
                arguments = @{
                    _vulcan = @{
                        contract = $Contract
                        project_id = $Project.project_id
                        pwd = $Project.pwd
                        session_id = 'windows-e2e-session'
                    }
                }
            }
        }
        Assert-E2e -Condition (-not $Query.result.isError) `
            -Message "Managed query routing failed for $($Project.project_id)."

        # SearchResponse verifies that isolated managed processes can execute Windows text search without PATH inheritance.
        # SearchResponse 验证隔离的托管进程无需继承 PATH 也能执行 Windows 文本搜索。
        $SearchResponse = Invoke-McpRequest -Process $Process -TimeoutMs $ResponseTimeoutMs -Request @{
            jsonrpc = '2.0'
            id = 500
            method = 'tools/call'
            params = @{
                name = 'search_code'
                arguments = @{
                    pattern = 'int main'
                    _vulcan = @{
                        contract = $Contract
                        project_id = $Project.project_id
                        pwd = $Project.pwd
                        session_id = 'windows-e2e-session'
                    }
                }
            }
        }
        Assert-E2e -Condition (-not $SearchResponse.result.isError) `
            -Message "Managed text search routing failed for $($Project.project_id)."
        Assert-E2e -Condition ($SearchResponse.result.content[0].text -match 'total_grep_matches:\s*[1-9]') `
            -Message "Managed Windows text search returned no grep matches for $($Project.project_id)."
    }

    $Process.StandardInput.Close()
    Assert-E2e -Condition $Process.WaitForExit(10000) `
        -Message 'Managed MCP frontend did not exit after stdin closed.'
    Assert-E2e -Condition ($Process.ExitCode -eq 0) `
        -Message "Managed MCP frontend exited with code $($Process.ExitCode)."

    # The managed daemon is session-independent while the frontend is alive, but application-scoped.
    # 托管 daemon 在前端存活期间独立于业务会话，但生命周期从属于应用。
    $DaemonExitDeadline = [DateTime]::UtcNow.AddSeconds(10)
    do {
        $CreatedDaemons = Get-ManagedDaemonProcess -Path $CanonicalExecutable
        $CreatedDaemonIds = @($CreatedDaemons | ForEach-Object { [int]$_.ProcessId })
        if ($CreatedDaemonIds.Count -eq 0) {
            break
        }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $DaemonExitDeadline)
    Assert-E2e -Condition ($CreatedDaemonIds.Count -eq 0) `
        -Message 'Application-scoped managed daemon survived the final MCP frontend session.'

    Write-Host 'Vulcan managed Windows E2E passed: 3 projects, hidden routing, app-scoped daemon.'
}
finally {
    if ($Process -and -not $Process.HasExited) {
        $Process.Kill($true)
        [void]$Process.WaitForExit(5000)
    }

    # Rediscover the exact managed daemon so failures before lifecycle verification cannot leak a test process.
    # 重新发现精确的托管 daemon，避免生命周期验证前失败时泄漏测试进程。
    $CleanupDaemonIds = @($CreatedDaemonIds)
    try {
        $CleanupDaemonIds += @(Get-ManagedDaemonProcess -Path $CanonicalExecutable |
                ForEach-Object { [int]$_.ProcessId })
    }
    catch {
        Write-Warning "Unable to rediscover managed daemon processes during cleanup: $_"
    }
    foreach ($DaemonId in @($CleanupDaemonIds | Sort-Object -Unique)) {
        $Daemon = Get-Process -Id $DaemonId -ErrorAction SilentlyContinue
        if ($Daemon) {
            Stop-Process -Id $DaemonId -Force
            [void]$Daemon.WaitForExit(5000)
        }
    }
    Remove-TestTree -Path $TestRoot
}
