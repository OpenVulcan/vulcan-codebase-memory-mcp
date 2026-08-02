param(
    # Managed product executable to verify.
    # 要验证的托管产品可执行文件。
    [string]$Executable = (Join-Path $PSScriptRoot '..\build\c\vulcan-codebase-memory-mcp.exe'),

    # Maximum wait for one MCP response.
    # 单次 MCP 响应的最长等待时间。
    [int]$ResponseTimeoutMs = 30000
)

$ErrorActionPreference = 'Stop'
$Contract = 'vulcan.codebase-memory/2'

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
    return $Line | ConvertFrom-Json
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
Waits for one exact managed index mode to reach a successful terminal snapshot.
等待一种精确的托管索引模式到达成功终态快照。
#>
function Wait-ManagedIndexJob {
    param(
        # Running MCP frontend process.
        # 正在运行的 MCP 前端进程。
        [Parameter(Mandatory)]
        [System.Diagnostics.Process]$Process,

        # Canonical managed contract.
        # 规范化托管契约。
        [Parameter(Mandatory)]
        [string]$Contract,

        # Managed project identity.
        # 托管项目身份。
        [Parameter(Mandatory)]
        [hashtable]$Project,

        # Expected exact index mode.
        # 预期的精确索引模式。
        [Parameter(Mandatory)]
        [ValidateSet('update', 'rebuild')]
        [string]$Mode,

        # Minimum revision required after publication.
        # 发布后要求的最小修订号。
        [Parameter(Mandatory)]
        [long]$MinimumRevision,

        # Maximum wait for one MCP response.
        # 单次 MCP 响应的最长等待时间。
        [Parameter(Mandatory)]
        [int]$TimeoutMs
    )

    # JobIdentity records the first exact identity observed for this request.
    # JobIdentity 记录本次请求首次观察到的精确任务身份。
    $JobIdentity = $null
    for ($Attempt = 0; $Attempt -lt 400; $Attempt++) {
        $Status = Invoke-McpRequest -Process $Process -TimeoutMs $TimeoutMs -Request @{
            jsonrpc = '2.0'
            id = 700 + $Attempt
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
        if ($StatusBody.last_error) {
            throw "Managed indexing failed for $($Project.project_id): $($StatusBody.last_error.code) - $($StatusBody.last_error.message)"
        }

        # CandidateJob prefers the live job and then the immutable terminal snapshot.
        # CandidateJob 优先使用实时任务，其次使用不可变终态快照。
        $CandidateJob = if ($StatusBody.active_job -and $StatusBody.active_job.mode -eq $Mode) {
            $StatusBody.active_job
        }
        elseif ($StatusBody.last_job -and $StatusBody.last_job.mode -eq $Mode) {
            $StatusBody.last_job
        }
        else {
            $null
        }
        if ($CandidateJob) {
            $CandidateIdentity = "$($CandidateJob.runtime_id):$($CandidateJob.job_id):$($CandidateJob.job_generation)"
            if (-not $JobIdentity) {
                $JobIdentity = $CandidateIdentity
            }
            Assert-E2e -Condition ($CandidateIdentity -eq $JobIdentity) `
                -Message "Managed index job identity changed while polling $Mode."
            Assert-E2e -Condition ($CandidateJob.completed_units -ge 0) `
                -Message "Managed index job reported an invalid completed unit count for $Mode."
        }

        if ($StatusBody.last_job -and
            $StatusBody.last_job.mode -eq $Mode -and
            $StatusBody.last_job.state -eq 'succeeded' -and
            $StatusBody.revision -ge $MinimumRevision) {
            Assert-E2e -Condition ($StatusBody.last_job.trigger -eq 'manual') `
                -Message "Managed index job returned the wrong trigger for $Mode."
            Assert-E2e -Condition ($StatusBody.last_job.phase -eq 'finalizing') `
                -Message "Managed index job returned the wrong terminal phase for $Mode."
            Assert-E2e -Condition ($null -eq $StatusBody.active_job) `
                -Message "Managed index job remained active after successful $Mode publication."
            Assert-E2e -Condition ($StatusBody.availability -in @('ready', 'degraded')) `
                -Message "Managed project did not remain queryable after $Mode publication."
            Assert-E2e -Condition (-not [string]::IsNullOrWhiteSpace($JobIdentity)) `
                -Message "Managed index job identity was never observable for $Mode."
            return $StatusBody
        }
        Start-Sleep -Milliseconds 50
    }
    throw "Managed index job did not complete: $($Project.project_id) mode=$Mode"
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
        # Utf8NoBomEncoding keeps the fixture identical on Windows PowerShell and PowerShell 7.
        # Utf8NoBomEncoding 确保夹具在 Windows PowerShell 与 PowerShell 7 下完全一致。
        $Utf8NoBomEncoding = [System.Text.UTF8Encoding]::new($false)
        [System.IO.File]::WriteAllText(
            (Join-Path $ProjectRoot 'main.c'),
            'int main(void) { return 0; }',
            $Utf8NoBomEncoding
        )
    }

    $Start = [System.Diagnostics.ProcessStartInfo]::new()
    $Start.FileName = $CanonicalExecutable
    $Start.Arguments = '--vulcan-managed'
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
            'vulcan_update_project_index', 'get_graph_schema')) {
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
            if ($StatusBody.last_error) {
                throw "Managed indexing failed for $($Project.project_id): $($StatusBody.last_error.code)"
            }
            $Ready = $StatusBody.availability -in @('ready', 'degraded') -and
                $null -eq $StatusBody.active_job -and $StatusBody.revision -gt 0
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

    # Exercise both explicit modes against the first published project.
    # 针对首个已发布项目实际执行两种显式模式。
    $ControlledProject = $Projects[0]
    $InitialStatus = Invoke-McpRequest -Process $Process -TimeoutMs $ResponseTimeoutMs -Request @{
        jsonrpc = '2.0'
        id = 600
        method = 'tools/call'
        params = @{
            name = 'vulcan_project_status'
            arguments = @{
                contract = $Contract
                project_id = $ControlledProject.project_id
                pwd = $ControlledProject.pwd
            }
        }
    }
    $InitialStatusBody = $InitialStatus.result.content[0].text | ConvertFrom-Json
    $InitialRevision = [long]$InitialStatusBody.revision
    [System.IO.File]::AppendAllText(
        (Join-Path $ControlledProject.pwd 'main.c'),
        "`nint managed_update_probe(void) { return 1; }",
        [System.Text.UTF8Encoding]::new($false)
    )

    $Update = Invoke-McpRequest -Process $Process -TimeoutMs $ResponseTimeoutMs -Request @{
        jsonrpc = '2.0'
        id = 601
        method = 'tools/call'
        params = @{
            name = 'vulcan_update_project_index'
            arguments = @{
                contract = $Contract
                project_id = $ControlledProject.project_id
                pwd = $ControlledProject.pwd
                mode = 'update'
            }
        }
    }
    $UpdateBody = $Update.result.content[0].text | ConvertFrom-Json
    Assert-E2e -Condition ($UpdateBody.accepted -and $UpdateBody.mode -eq 'update') `
        -Message 'Managed update request was not accepted with the exact mode.'
    $UpdatedStatus = Wait-ManagedIndexJob -Process $Process -Contract $Contract `
        -Project $ControlledProject -Mode 'update' -MinimumRevision ($InitialRevision + 1) `
        -TimeoutMs $ResponseTimeoutMs

    $Rebuild = Invoke-McpRequest -Process $Process -TimeoutMs $ResponseTimeoutMs -Request @{
        jsonrpc = '2.0'
        id = 602
        method = 'tools/call'
        params = @{
            name = 'vulcan_update_project_index'
            arguments = @{
                contract = $Contract
                project_id = $ControlledProject.project_id
                pwd = $ControlledProject.pwd
                mode = 'rebuild'
            }
        }
    }
    $RebuildBody = $Rebuild.result.content[0].text | ConvertFrom-Json
    Assert-E2e -Condition ($RebuildBody.accepted -and $RebuildBody.mode -eq 'rebuild') `
        -Message 'Managed rebuild request was not accepted with the exact mode.'
    $RebuiltStatus = Wait-ManagedIndexJob -Process $Process -Contract $Contract `
        -Project $ControlledProject -Mode 'rebuild' -MinimumRevision ([long]$UpdatedStatus.revision + 1) `
        -TimeoutMs $ResponseTimeoutMs
    Assert-E2e -Condition ([long]$RebuiltStatus.revision -gt [long]$UpdatedStatus.revision) `
        -Message 'Managed rebuild did not publish a newer revision.'

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

    Write-Host 'Vulcan managed Windows E2E passed: 3 projects, V2 update/rebuild progress, hidden routing, app-scoped daemon.'
}
finally {
    if ($Process -and -not $Process.HasExited) {
        $Process.Kill()
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
