#Requires -Version 5.1
<#
    tickets.ps1 - file-based ticket tracker for XMB.

    One markdown file per ticket in tickets/, flat "key: value" frontmatter.
    No external modules, no network, no external dependencies.

    Usage:
      scripts/tickets.ps1 new -Type bug -Title "..." -Priority P1 -Area ui
      scripts/tickets.ps1 set -Id XMB-0042 -Status fixed -Commits abc1234 -Note "..."
      scripts/tickets.ps1 list [-Status open] [-Type bug] [-Area ui] [-Priority P0]
      scripts/tickets.ps1 checklist
      scripts/tickets.ps1 board
      scripts/tickets.ps1 validate

    Docs: tickets/README.md
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string]$Command = 'help',

    [string]$Id,
    [string]$Type,
    [string]$Title,
    [string]$Priority,
    [string]$Area,
    [string]$Source,
    [string]$VerifyIn,
    [string]$Slug,
    [string]$Status,
    [string]$Commits,
    [string]$Links,
    [string]$Labels,
    [string]$Note,
    [string]$Path,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------- constants --

$TypeValues = @('bug', 'feature', 'tech', 'decision', 'question')
$StatusValues = @('open', 'in-progress', 'fixed', 'verified', 'closed', 'wontfix', 'cannot-reproduce', 'accepted', 'superseded')
$PriorityValues = @('P0', 'P1', 'P2', 'P3')
$AreaValues = @('ui', 'player', 'audio', 'library', 'net', 'power', 'display', 'input', 'firmware', 'tools', 'docs', 'infra')
$VerifyValues = @('review', 'test', 'build', 'none')

# Statuses that end a ticket's life; changing them requires -Force.
$TerminalStatuses = @('verified', 'closed', 'wontfix', 'superseded')
# Statuses that mean "still on the board".
$ActiveStatuses = @('open', 'in-progress', 'fixed')
# decision tickets live in their own two-state world.
$DecisionStatuses = @('accepted', 'superseded')

$FieldOrder = @('id', 'title', 'type', 'status', 'priority', 'area', 'source', 'verify-in', 'created', 'updated', 'links', 'commits', 'labels')
$RequiredFields = @('id', 'title', 'type', 'status', 'priority', 'area', 'source', 'created', 'updated')

$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)

# ------------------------------------------------------------------ helpers --

function Die([string]$message) {
    [Console]::Error.WriteLine("ERROR: $message")
    exit 1
}

function Get-RepoRoot {
    return (Split-Path -Parent $PSScriptRoot)
}

function Get-TicketsDir {
    $dir = Join-Path (Get-RepoRoot) 'tickets'
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
    return $dir
}

function Read-TextFile([string]$path) {
    return [IO.File]::ReadAllText($path, [Text.Encoding]::UTF8)
}

function Write-TextFile([string]$path, [string]$text) {
    [IO.File]::WriteAllText($path, $text, $Utf8NoBom)
}

function Today {
    return (Get-Date -Format 'yyyy-MM-dd')
}

function Test-OneOf([string]$value, [string[]]$allowed) {
    return ($allowed -contains $value)
}

function Join-Allowed([string[]]$allowed) {
    return ($allowed -join ' | ')
}

# Cyrillic -> latin, for file name slugs.
$TranslitMap = @{
    'а' = 'a'; 'б' = 'b'; 'в' = 'v'; 'г' = 'g'; 'д' = 'd'; 'е' = 'e'; 'ё' = 'e'; 'ж' = 'zh'
    'з' = 'z'; 'и' = 'i'; 'й' = 'y'; 'к' = 'k'; 'л' = 'l'; 'м' = 'm'; 'н' = 'n'; 'о' = 'o'
    'п' = 'p'; 'р' = 'r'; 'с' = 's'; 'т' = 't'; 'у' = 'u'; 'ф' = 'f'; 'х' = 'h'; 'ц' = 'ts'
    'ч' = 'ch'; 'ш' = 'sh'; 'щ' = 'sch'; 'ъ' = ''; 'ы' = 'y'; 'ь' = ''; 'э' = 'e'; 'ю' = 'yu'
    'я' = 'ya'
}

function New-Slug([string]$text) {
    $lower = $text.ToLowerInvariant()
    $sb = New-Object System.Text.StringBuilder
    foreach ($ch in $lower.ToCharArray()) {
        $key = [string]$ch
        if ($TranslitMap.ContainsKey($key)) {
            [void]$sb.Append($TranslitMap[$key])
        }
        elseif ($ch -match '[a-z0-9]') {
            [void]$sb.Append($ch)
        }
        else {
            [void]$sb.Append('-')
        }
    }
    $slug = $sb.ToString()
    $slug = $slug -replace '-+', '-'
    $slug = $slug.Trim('-')
    if ($slug.Length -gt 48) {
        $slug = $slug.Substring(0, 48).Trim('-')
        # do not cut a word in half if we can avoid it
        $lastDash = $slug.LastIndexOf('-')
        if ($lastDash -gt 24) { $slug = $slug.Substring(0, $lastDash) }
    }
    if ($slug -eq '') { $slug = 'ticket' }
    return $slug
}

# ------------------------------------------------------------------- parsing --

function Read-Ticket([string]$path) {
    $text = Read-TextFile $path
    $lines = $text -split "`r?`n"

    $fields = [ordered]@{}
    $bodyStart = 0
    $errors = @()

    if ($lines.Length -lt 2 -or $lines[0].Trim() -ne '---') {
        $errors += 'no frontmatter (file must start with ---)'
    }
    else {
        $closed = $false
        for ($i = 1; $i -lt $lines.Length; $i++) {
            $line = $lines[$i]
            if ($line.Trim() -eq '---') { $bodyStart = $i + 1; $closed = $true; break }
            if ($line.Trim() -eq '') { continue }
            if ($line -match '^([a-z][a-z-]*):\s*(.*)$') {
                $fields[$Matches[1]] = $Matches[2].Trim()
            }
            else {
                $errors += "unparsable frontmatter line $($i + 1): $line"
            }
        }
        if (-not $closed) { $errors += 'frontmatter is not closed by ---' }
    }

    $body = ''
    if ($bodyStart -gt 0 -and $bodyStart -lt $lines.Length) {
        $body = ($lines[$bodyStart..($lines.Length - 1)] -join "`n")
    }

    return [pscustomobject]@{
        Path        = $path
        FileName    = (Split-Path -Leaf $path)
        Fields      = $fields
        Body        = $body
        ParseErrors = $errors
    }
}

function Save-Ticket($ticket) {
    $sb = New-Object System.Text.StringBuilder
    [void]$sb.Append("---`n")
    # known fields first, in canonical order, then anything unexpected
    foreach ($key in $FieldOrder) {
        if ($ticket.Fields.Contains($key)) {
            [void]$sb.Append("$key" + ': ' + $ticket.Fields[$key] + "`n")
        }
    }
    foreach ($key in $ticket.Fields.Keys) {
        if ($FieldOrder -notcontains $key) {
            [void]$sb.Append("$key" + ': ' + $ticket.Fields[$key] + "`n")
        }
    }
    [void]$sb.Append("---`n")
    $body = $ticket.Body
    if (-not $body.StartsWith("`n")) { [void]$sb.Append("`n") }
    [void]$sb.Append($body)
    $text = $sb.ToString()
    if (-not $text.EndsWith("`n")) { $text += "`n" }
    Write-TextFile $ticket.Path $text
}

function Get-AllTickets {
    $dir = Get-TicketsDir
    $files = Get-ChildItem -Path $dir -Filter 'XMB-*.md' -File | Sort-Object Name
    $result = @()
    foreach ($f in $files) { $result += (Read-Ticket $f.FullName) }
    return $result
}

function Find-Ticket([string]$id) {
    if ([string]::IsNullOrWhiteSpace($id)) { Die 'missing -Id' }
    $id = $id.ToUpperInvariant()
    $dir = Get-TicketsDir
    $matched = Get-ChildItem -Path $dir -Filter "$id-*.md" -File
    if ($matched.Count -eq 0) { Die "ticket $id not found in $dir" }
    if ($matched.Count -gt 1) { Die "several files for $id : $($matched.Name -join ', ')" }
    return (Read-Ticket $matched[0].FullName)
}

function Get-NextId {
    $max = 0
    foreach ($t in (Get-AllTickets)) {
        if ($t.FileName -match '^XMB-(\d+)-') {
            $n = [int]$Matches[1]
            if ($n -gt $max) { $max = $n }
        }
    }
    return ($max + 1)
}

function Add-HistoryLine($ticket, [string]$line) {
    $stamp = "- $(Today) (agent): $line"
    if ($ticket.Body -match '(?m)^##\s+История\s*$') {
        # append at the very end of the file (История is always the last section)
        $body = $ticket.Body.TrimEnd()
        if ($body -match '(?m)^##\s+История\s*$' -and $body.TrimEnd().EndsWith('## История')) {
            $ticket.Body = $body + "`n`n" + $stamp + "`n"   # section was still empty
        }
        else {
            $ticket.Body = $body + "`n" + $stamp + "`n"
        }
    }
    else {
        $ticket.Body = $ticket.Body.TrimEnd() + "`n`n## История`n`n" + $stamp + "`n"
    }
}

# ----------------------------------------------------------------- templates --

function New-TicketBody([string]$type) {
    if ($type -eq 'decision') {
        return @"

## Решение

<что решили, одним-двумя абзацами>

## Контекст

<почему встал вопрос; ссылки на NOTES.md / тикеты>

## Альтернативы

<что ещё рассматривали и почему отклонили; убрать секцию, если альтернатив не было>

## Последствия

<что теперь обязаны соблюдать код и агенты>

## История

"@
    }
    if ($type -eq 'bug') {
        return @"

## Описание

<наблюдаемое поведение и ожидаемое>

## Репро

<шаги; сценарий XMB_SCRIPT или замер, если есть>

## Критерий закрытия

<что должно быть правдой, чтобы тикет закрыть>

## История

"@
    }
    return @"

## Описание

<суть задачи>

## Критерий закрытия

<что должно быть правдой, чтобы тикет закрыть>

## История

"@
}

# ---------------------------------------------------------------- validation --

function Test-Ticket($ticket) {
    $problems = @()
    foreach ($e in $ticket.ParseErrors) { $problems += $e }

    $f = $ticket.Fields
    foreach ($key in $RequiredFields) {
        if (-not $f.Contains($key) -or [string]::IsNullOrWhiteSpace($f[$key])) {
            $problems += "missing required field '$key'"
        }
    }
    if ($problems.Count -gt 0 -and -not $f.Contains('id')) {
        return $problems
    }

    if ($f.Contains('id')) {
        if ($f['id'] -notmatch '^XMB-\d{4}$') {
            $problems += "id '$($f['id'])' must look like XMB-0042"
        }
        elseif (-not $ticket.FileName.StartsWith($f['id'] + '-')) {
            $problems += "id '$($f['id'])' does not match file name '$($ticket.FileName)'"
        }
    }
    if ($f.Contains('type') -and -not (Test-OneOf $f['type'] $TypeValues)) {
        $problems += "type '$($f['type'])' is not one of: $(Join-Allowed $TypeValues)"
    }
    if ($f.Contains('status') -and -not (Test-OneOf $f['status'] $StatusValues)) {
        $problems += "status '$($f['status'])' is not one of: $(Join-Allowed $StatusValues)"
    }
    if ($f.Contains('priority') -and -not (Test-OneOf $f['priority'] $PriorityValues)) {
        $problems += "priority '$($f['priority'])' is not one of: $(Join-Allowed $PriorityValues)"
    }
    if ($f.Contains('area') -and -not (Test-OneOf $f['area'] $AreaValues)) {
        $problems += "area '$($f['area'])' is not one of: $(Join-Allowed $AreaValues)"
    }
    if ($f.Contains('verify-in') -and $f['verify-in'] -ne '' -and -not (Test-OneOf $f['verify-in'] $VerifyValues)) {
        $problems += "verify-in '$($f['verify-in'])' is not one of: $(Join-Allowed $VerifyValues)"
    }
    if ($f.Contains('type') -and $f.Contains('verify-in')) {
        if (($f['type'] -eq 'bug' -or $f['type'] -eq 'feature') -and [string]::IsNullOrWhiteSpace($f['verify-in'])) {
            $problems += "verify-in is required for type '$($f['type'])'"
        }
    }
    elseif ($f.Contains('type') -and ($f['type'] -eq 'bug' -or $f['type'] -eq 'feature')) {
        $problems += "verify-in is required for type '$($f['type'])'"
    }
    if ($f.Contains('source') -and $f['source'] -notmatch '^(agent|user|review-\d{4}-\d{2}-\d{2})$') {
        $problems += "source '$($f['source'])' must be agent, user or review-YYYY-MM-DD"
    }
    foreach ($dateField in @('created', 'updated')) {
        if ($f.Contains($dateField) -and $f[$dateField] -notmatch '^\d{4}-\d{2}-\d{2}$') {
            $problems += "$dateField '$($f[$dateField])' must be YYYY-MM-DD"
        }
    }
    if ($f.Contains('type') -and $f.Contains('status')) {
        if ($f['type'] -eq 'decision' -and -not (Test-OneOf $f['status'] $DecisionStatuses)) {
            $problems += "decision tickets may only be $(Join-Allowed $DecisionStatuses), got '$($f['status'])'"
        }
        if ($f['type'] -ne 'decision' -and (Test-OneOf $f['status'] $DecisionStatuses)) {
            $problems += "status '$($f['status'])' is only for decision tickets"
        }
    }
    if ($ticket.Body -notmatch '(?m)^##\s+История\s*$') {
        $problems += "no '## История' section"
    }
    return $problems
}

function Invoke-Validate {
    $tickets = @(Get-AllTickets)
    $failed = 0
    $seenIds = @{}

    foreach ($t in $tickets) {
        $problems = @(Test-Ticket $t)
        if ($t.Fields.Contains('id')) {
            $tid = $t.Fields['id']
            if ($seenIds.ContainsKey($tid)) {
                $problems += "duplicate id, also used by $($seenIds[$tid])"
            }
            else {
                $seenIds[$tid] = $t.FileName
            }
        }
        if ($problems.Count -gt 0) {
            $failed++
            Write-Host "FAIL $($t.FileName)" -ForegroundColor Red
            foreach ($p in $problems) { Write-Host "     - $p" }
        }
    }

    # links can only be checked once every id is known
    foreach ($t in $tickets) {
        if (-not $t.Fields.Contains('links')) { continue }
        $raw = $t.Fields['links']
        if ([string]::IsNullOrWhiteSpace($raw)) { continue }
        $broken = @()
        foreach ($ref in ($raw -split ',')) {
            $ref = $ref.Trim()
            if ($ref -eq '') { continue }
            if (-not $seenIds.ContainsKey($ref)) { $broken += $ref }
        }
        if ($broken.Count -gt 0) {
            $failed++
            Write-Host "FAIL $($t.FileName)" -ForegroundColor Red
            Write-Host "     - links point at missing tickets: $($broken -join ', ')"
        }
    }

    if ($failed -gt 0) {
        Write-Host ''
        Write-Host "$failed failing ticket(s) out of $($tickets.Count)" -ForegroundColor Red
        exit 1
    }
    Write-Host "OK: $($tickets.Count) ticket(s) valid" -ForegroundColor Green
}

# ------------------------------------------------------------------ commands --

function Invoke-New {
    if ([string]::IsNullOrWhiteSpace($Title)) { Die 'missing -Title' }
    if ([string]::IsNullOrWhiteSpace($Type)) { Die "missing -Type ($(Join-Allowed $TypeValues))" }
    if (-not (Test-OneOf $Type $TypeValues)) { Die "-Type must be one of: $(Join-Allowed $TypeValues)" }

    $prio = $Priority
    if ([string]::IsNullOrWhiteSpace($prio)) { $prio = 'P2' }
    if (-not (Test-OneOf $prio $PriorityValues)) { Die "-Priority must be one of: $(Join-Allowed $PriorityValues)" }

    if ([string]::IsNullOrWhiteSpace($Area)) { Die "missing -Area ($(Join-Allowed $AreaValues))" }
    if (-not (Test-OneOf $Area $AreaValues)) { Die "-Area must be one of: $(Join-Allowed $AreaValues)" }

    $src = $Source
    if ([string]::IsNullOrWhiteSpace($src)) { $src = 'agent' }
    if ($src -notmatch '^(agent|user|review-\d{4}-\d{2}-\d{2})$') {
        Die '-Source must be agent, user or review-YYYY-MM-DD'
    }

    $verify = $VerifyIn
    if ([string]::IsNullOrWhiteSpace($verify)) {
        if ($Type -eq 'bug') { $verify = 'review' }
        elseif ($Type -eq 'feature') { $verify = 'review' }
        else { $verify = 'none' }
    }
    if (-not (Test-OneOf $verify $VerifyValues)) { Die "-VerifyIn must be one of: $(Join-Allowed $VerifyValues)" }

    $st = $Status
    if ([string]::IsNullOrWhiteSpace($st)) {
        if ($Type -eq 'decision') { $st = 'accepted' } else { $st = 'open' }
    }
    if (-not (Test-OneOf $st $StatusValues)) { Die "-Status must be one of: $(Join-Allowed $StatusValues)" }

    $num = Get-NextId
    $id = 'XMB-{0:D4}' -f $num
    $slugText = $Slug
    if ([string]::IsNullOrWhiteSpace($slugText)) { $slugText = $Title }
    $fileSlug = New-Slug $slugText
    $path = Join-Path (Get-TicketsDir) ("$id-$fileSlug.md")

    $fields = [ordered]@{}
    $fields['id'] = $id
    $fields['title'] = $Title
    $fields['type'] = $Type
    $fields['status'] = $st
    $fields['priority'] = $prio
    $fields['area'] = $Area
    $fields['source'] = $src
    $fields['verify-in'] = $verify
    $fields['created'] = (Today)
    $fields['updated'] = (Today)
    if (-not [string]::IsNullOrWhiteSpace($Links)) { $fields['links'] = $Links }
    if (-not [string]::IsNullOrWhiteSpace($Commits)) { $fields['commits'] = $Commits }
    if (-not [string]::IsNullOrWhiteSpace($Labels)) { $fields['labels'] = $Labels }

    $ticket = [pscustomobject]@{
        Path        = $path
        FileName    = (Split-Path -Leaf $path)
        Fields      = $fields
        Body        = (New-TicketBody $Type)
        ParseErrors = @()
    }

    $firstNote = $Note
    if ([string]::IsNullOrWhiteSpace($firstNote)) { $firstNote = "создан ($Type, $st)." }
    Add-HistoryLine $ticket $firstNote
    Save-Ticket $ticket

    Write-Host "created $id" -ForegroundColor Green
    Write-Output $path   # stdout, so callers can script against it
}

function Invoke-Set {
    $ticket = Find-Ticket $Id
    $f = $ticket.Fields
    $changes = @()

    if (-not [string]::IsNullOrWhiteSpace($Status)) {
        if (-not (Test-OneOf $Status $StatusValues)) { Die "-Status must be one of: $(Join-Allowed $StatusValues)" }
        $old = ''
        if ($f.Contains('status')) { $old = $f['status'] }
        if ($old -ne $Status) {
            if ((Test-OneOf $old $TerminalStatuses) -and -not $Force) {
                Die "$($f['id']) is $old (terminal). Re-open only with -Force."
            }
            if ($f['type'] -eq 'decision' -and -not (Test-OneOf $Status $DecisionStatuses)) {
                Die "decision tickets may only be $(Join-Allowed $DecisionStatuses)"
            }
            if ($f['type'] -ne 'decision' -and (Test-OneOf $Status $DecisionStatuses)) {
                Die "status '$Status' is only for decision tickets"
            }
            $f['status'] = $Status
            $changes += "status $old -> $Status"
        }
    }
    if (-not [string]::IsNullOrWhiteSpace($Priority)) {
        if (-not (Test-OneOf $Priority $PriorityValues)) { Die "-Priority must be one of: $(Join-Allowed $PriorityValues)" }
        if ($f['priority'] -ne $Priority) {
            $changes += "priority $($f['priority']) -> $Priority"
            $f['priority'] = $Priority
        }
    }
    if (-not [string]::IsNullOrWhiteSpace($Area)) {
        if (-not (Test-OneOf $Area $AreaValues)) { Die "-Area must be one of: $(Join-Allowed $AreaValues)" }
        if ($f['area'] -ne $Area) {
            $changes += "area $($f['area']) -> $Area"
            $f['area'] = $Area
        }
    }
    if (-not [string]::IsNullOrWhiteSpace($VerifyIn)) {
        if (-not (Test-OneOf $VerifyIn $VerifyValues)) { Die "-VerifyIn must be one of: $(Join-Allowed $VerifyValues)" }
        $f['verify-in'] = $VerifyIn
        $changes += "verify-in -> $VerifyIn"
    }
    if (-not [string]::IsNullOrWhiteSpace($Title)) {
        $f['title'] = $Title
        $changes += 'title updated'
    }
    if (-not [string]::IsNullOrWhiteSpace($Commits)) {
        $existing = ''
        if ($f.Contains('commits')) { $existing = $f['commits'] }
        $list = @()
        foreach ($c in ($existing -split ',')) { if ($c.Trim() -ne '') { $list += $c.Trim() } }
        foreach ($c in ($Commits -split ',')) {
            $c = $c.Trim()
            if ($c -ne '' -and $list -notcontains $c) { $list += $c }
        }
        $f['commits'] = ($list -join ', ')
        $changes += "commits $($f['commits'])"
    }
    if (-not [string]::IsNullOrWhiteSpace($Links)) {
        $existing = ''
        if ($f.Contains('links')) { $existing = $f['links'] }
        $list = @()
        foreach ($c in ($existing -split ',')) { if ($c.Trim() -ne '') { $list += $c.Trim() } }
        foreach ($c in ($Links -split ',')) {
            $c = $c.Trim().ToUpperInvariant()
            if ($c -ne '' -and $list -notcontains $c) { $list += $c }
        }
        $f['links'] = ($list -join ', ')
        $changes += "links $($f['links'])"
    }
    if (-not [string]::IsNullOrWhiteSpace($Labels)) {
        $f['labels'] = $Labels
        $changes += "labels $Labels"
    }

    if ($changes.Count -eq 0 -and [string]::IsNullOrWhiteSpace($Note)) {
        Die 'nothing to change: pass -Status/-Priority/-Area/-VerifyIn/-Title/-Commits/-Links/-Labels or -Note'
    }

    $line = ''
    if ($changes.Count -gt 0) { $line = ($changes -join '; ') }
    if (-not [string]::IsNullOrWhiteSpace($Note)) {
        if ($line -ne '') { $line = "$line - $Note" } else { $line = $Note }
    }
    Add-HistoryLine $ticket $line
    $f['updated'] = (Today)

    $problems = @(Test-Ticket $ticket)
    if ($problems.Count -gt 0) {
        Write-Host "refusing to save $($f['id']):" -ForegroundColor Red
        foreach ($p in $problems) { Write-Host "  - $p" }
        exit 1
    }
    Save-Ticket $ticket
    Write-Host "$($f['id']): $line" -ForegroundColor Green
}

function Select-Tickets {
    $tickets = Get-AllTickets
    $out = @()
    foreach ($t in $tickets) {
        if ($t.Fields.Count -eq 0) { continue }
        if (-not [string]::IsNullOrWhiteSpace($Status)) {
            if ($t.Fields['status'] -ne $Status) { continue }
        }
        else {
            if (-not (Test-OneOf $t.Fields['status'] $ActiveStatuses) -and $t.Fields['status'] -ne 'accepted') { continue }
        }
        if (-not [string]::IsNullOrWhiteSpace($Type) -and $t.Fields['type'] -ne $Type) { continue }
        if (-not [string]::IsNullOrWhiteSpace($Area) -and $t.Fields['area'] -ne $Area) { continue }
        if (-not [string]::IsNullOrWhiteSpace($Priority) -and $t.Fields['priority'] -ne $Priority) { continue }
        $out += $t
    }
    return $out
}

function Invoke-List {
    $tickets = @(Select-Tickets | Sort-Object { $_.Fields['priority'] }, { $_.Fields['id'] })
    if ($tickets.Count -eq 0) {
        Write-Host 'no tickets match'
        return
    }
    Write-Host ('{0,-10} {1,-3} {2,-16} {3,-11} {4}' -f 'ID', 'P', 'STATUS', 'AREA', 'TITLE')
    Write-Host ('-' * 100)
    foreach ($t in $tickets) {
        $f = $t.Fields
        Write-Host ('{0,-10} {1,-3} {2,-16} {3,-11} {4}' -f $f['id'], $f['priority'], $f['status'], $f['area'], $f['title'])
    }
    Write-Host ''
    Write-Host "$($tickets.Count) ticket(s)"
}

function Get-PendingVerification {
    $out = @()
    foreach ($t in (Get-AllTickets)) {
        if ($t.Fields.Count -eq 0) { continue }
        if ($t.Fields['status'] -ne 'fixed') { continue }
        $out += $t
    }
    return $out
}

function Format-Checklist {
    $lines = @()
    $pending = @(Get-PendingVerification)
    if ($pending.Count -eq 0) {
        $lines += '_Нечего проверять: тикетов в статусе `fixed` нет._'
        return $lines
    }
    $groups = @(
        @{ Key = 'review'; Caption = 'Проверить НА ПЛЕЕРЕ' },
        @{ Key = 'build';    Caption = 'Проверить БИЛДОМ' },
        @{ Key = 'test';     Caption = 'Проверить ТЕСТОМ' },
        @{ Key = 'none';     Caption = 'Без проверки (можно закрывать)' }
    )
    foreach ($g in $groups) {
        $items = @()
        foreach ($t in $pending) {
            $v = ''
            if ($t.Fields.Contains('verify-in')) { $v = $t.Fields['verify-in'] }
            if ($v -eq $g.Key) { $items += $t }
        }
        if ($items.Count -eq 0) { continue }
        $lines += "**$($g.Caption)**"
        $lines += ''
        foreach ($t in ($items | Sort-Object { $_.Fields['priority'] }, { $_.Fields['id'] })) {
            $f = $t.Fields
            $lines += "- [ ] [$($f['id'])]($($t.FileName)) ``[$($f['priority'])/$($f['area'])]`` $($f['title'])"
        }
        $lines += ''
    }
    return $lines
}

function Invoke-Checklist {
    foreach ($line in (Format-Checklist)) { Write-Host $line }
}

function Resolve-FeedbackPath {
    # Where the board UI can realistically have put the file: next to the tickets
    # (File System Access API, the user picks it once) or in Downloads (the plain
    # download fallback). An explicit -Path always wins.
    if (-not [string]::IsNullOrWhiteSpace($Path)) {
        if (-not (Test-Path $Path)) { Die "no feedback file at '$Path'" }
        return (Resolve-Path $Path).Path
    }
    $candidates = @(
        (Join-Path (Get-TicketsDir) 'feedback.json'),
        (Join-Path $env:USERPROFILE 'Downloads/feedback.json')
    )
    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) { return (Resolve-Path $candidate).Path }
    }
    Die ("no feedback file found. Save it from board.html (the button " +
        "'Сохранить фидбек в файл'), or pass -Path. Looked in: " + ($candidates -join '; '))
}

# Applies a review report saved from board.html: verdicts, notes and new items.
#
# Closes the loop the user asked for (XMB-0074): mark verdicts in the UI, press one
# button, and they are applied without anything being copied through the chat. A
# browser cannot write into the repository, so the UI writes a JSON file and this
# reads it.
#
# Deliberate behaviours:
# - the file is ARCHIVED after a successful run, so the same verdicts cannot be
#   applied twice by accident;
# - an unknown ticket id is reported and skipped instead of aborting the batch: a
#   stale UI (a ticket deleted or renumbered) must not block the rest;
# - 'verified' is the one status the agent may never decide alone, and this file is
#   exactly where the user's own verdict arrives.
function Invoke-Apply {
    $file = Resolve-FeedbackPath
    $raw = Read-TextFile $file
    try { $report = $raw | ConvertFrom-Json } catch { Die "could not parse '$file': $_" }

    $stamp = (Today)
    $applied = 0
    $created = 0
    $skipped = @()

    if ($report.verdicts) {
        foreach ($entry in $report.verdicts.PSObject.Properties) {
            $id = $entry.Name
            $verdict = [string]$entry.Value
            if ([string]::IsNullOrWhiteSpace($verdict)) { continue }

            $ticket = Get-AllTickets | Where-Object { $_.Fields['id'] -eq $id } | Select-Object -First 1
            if (-not $ticket) { $skipped += "$id (no such ticket)"; continue }

            # Accepts every spelling the UI or a hand-written report can produce.
            # 'fail' is the one the board buttons actually store, and the first
            # version of this switch did not have it: a real report would have
            # silently skipped every FAILED verdict, i.e. lost exactly the half
            # that matters. Keep the aliases.
            $status = ''
            switch ($verdict.ToLowerInvariant()) {
                'ok' { $status = 'verified' }
                'verified' { $status = 'verified' }
                'pass' { $status = 'verified' }
                'fail' { $status = 'open' }
                'bad' { $status = 'open' }
                'open' { $status = 'open' }
            }
            if ([string]::IsNullOrWhiteSpace($status)) { $skipped += "$id (verdict '$verdict')"; continue }

            $note = ''
            if ($report.notes -and $report.notes.PSObject.Properties[$id]) {
                $note = [string]$report.notes.$id
            }
            if ($status -eq 'verified') { $line = "ревью ${stamp}: фикс работает" }
            else { $line = "ревью ${stamp}: фикс НЕ помог, проблема осталась" }
            if (-not [string]::IsNullOrWhiteSpace($note)) { $line = "$line — $note" }

            $ticket.Fields['status'] = $status
            Add-HistoryLine $ticket $line
            Save-Ticket $ticket
            Write-Host ("{0}: -> {1}" -f $id, $status) -ForegroundColor Green
            $applied++
        }
    }

    if ($report.feedback) {
        foreach ($item in $report.feedback) {
            if ([string]::IsNullOrWhiteSpace([string]$item.title)) { continue }
            $num = Get-NextId
            $newId = 'XMB-{0:D4}' -f $num
            $itemType = [string]$item.type
            if ([string]::IsNullOrWhiteSpace($itemType)) { $itemType = 'bug' }
            $itemArea = [string]$item.area
            if ([string]::IsNullOrWhiteSpace($itemArea)) { $itemArea = 'ui' }
            $itemPrio = [string]$item.prio
            if ([string]::IsNullOrWhiteSpace($itemPrio)) { $itemPrio = 'P2' }
            $verify = 'review'
            if ($itemType -eq 'tech' -or $itemType -eq 'decision' -or $itemType -eq 'question') { $verify = 'none' }
            $status = 'open'
            if ($itemType -eq 'decision') { $status = 'accepted' }

            $fields = [ordered]@{}
            $fields['id'] = $newId
            $fields['title'] = [string]$item.title
            $fields['type'] = $itemType
            $fields['status'] = $status
            $fields['priority'] = $itemPrio
            $fields['area'] = $itemArea
            $fields['source'] = "review-$stamp"
            $fields['verify-in'] = $verify
            $fields['created'] = $stamp
            $fields['updated'] = $stamp

            $newPath = Join-Path (Get-TicketsDir) ("$newId-" + (New-Slug ([string]$item.title)) + '.md')
            $ticket = [pscustomobject]@{
                Path        = $newPath
                FileName    = (Split-Path -Leaf $newPath)
                Fields      = $fields
                Body        = (New-TicketBody $itemType)
                ParseErrors = @()
            }
            $desc = [string]$item.desc
            if ([string]::IsNullOrWhiteSpace($desc)) { $firstNote = "из фидбека ревью $stamp" }
            else { $firstNote = "из фидбека ревью ${stamp}: $desc" }
            Add-HistoryLine $ticket $firstNote
            Save-Ticket $ticket
            Write-Host ("created {0}: {1}" -f $newId, $item.title) -ForegroundColor Green
            $created++
        }
    }

    foreach ($problem in $skipped) {
        Write-Host "skipped $problem" -ForegroundColor Yellow
    }

    if ($applied -eq 0 -and $created -eq 0) {
        Write-Host 'nothing to apply (no verdicts, no new items)' -ForegroundColor Yellow
        return
    }

    $archive = Join-Path (Get-TicketsDir) 'feedback.applied.json'
    Move-Item -Force -LiteralPath $file -Destination $archive
    Write-Host ("applied {0} verdict(s), created {1} ticket(s); report archived to {2}" -f $applied, $created, $archive) -ForegroundColor Cyan

    Invoke-Board
}

function Invoke-Board {
    $tickets = @(Get-AllTickets)
    $valid = @()
    foreach ($t in $tickets) {
        if ($t.Fields.Count -eq 0 -or -not $t.Fields.Contains('status')) { continue }
        $valid += $t
    }

    $lines = @()
    $lines += '<!-- GENERATED by scripts/tickets.ps1 board - НЕ РЕДАКТИРОВАТЬ РУКАМИ, правь тикеты -->'
    $lines += ''
    $lines += '# Доска тикетов XMB'
    $lines += ''
    $lines += "Сгенерировано $(Today). Формат и процессы — [tickets/README.md](README.md)."
    $lines += ''

    # 1. Summary
    $byStatus = @{}
    foreach ($s in $StatusValues) { $byStatus[$s] = 0 }
    foreach ($t in $valid) {
        $s = $t.Fields['status']
        if ($byStatus.ContainsKey($s)) { $byStatus[$s]++ }
    }
    $lines += '## Сводка'
    $lines += ''
    $lines += '| Статус | Тикетов |'
    $lines += '|---|---|'
    foreach ($s in $StatusValues) {
        if ($byStatus[$s] -gt 0) { $lines += "| ``$s`` | $($byStatus[$s]) |" }
    }
    $lines += "| **всего** | **$($valid.Count)** |"
    $lines += ''

    $areaRows = @()
    foreach ($a in $AreaValues) {
        $o = 0; $p = 0; $fx = 0
        foreach ($t in $valid) {
            if ($t.Fields['area'] -ne $a) { continue }
            if ($t.Fields['status'] -eq 'open') { $o++ }
            elseif ($t.Fields['status'] -eq 'in-progress') { $p++ }
            elseif ($t.Fields['status'] -eq 'fixed') { $fx++ }
        }
        if (($o + $p + $fx) -gt 0) { $areaRows += "| $a | $o | $p | $fx |" }
    }
    if ($areaRows.Count -gt 0) {
        $lines += '| Область | open | in-progress | fixed |'
        $lines += '|---|---|---|---|'
        $lines += $areaRows
        $lines += ''
    }

    # 2. Open by priority
    $lines += '## 🔥 Открытые (open / in-progress)'
    $lines += ''
    $openOnes = @()
    foreach ($t in $valid) {
        if ($t.Fields['status'] -eq 'open' -or $t.Fields['status'] -eq 'in-progress') { $openOnes += $t }
    }
    if ($openOnes.Count -eq 0) {
        $lines += '_Пусто._'
        $lines += ''
    }
    else {
        $lines += '| ID | P | Статус | Область | Заголовок |'
        $lines += '|---|---|---|---|---|'
        foreach ($t in ($openOnes | Sort-Object { $_.Fields['priority'] }, { $_.Fields['id'] })) {
            $f = $t.Fields
            $mark = ''
            if ($f.Contains('labels') -and $f['labels'] -match '(^|,\s*)watch(\s*,|$)') { $mark = ' 👀' }
            $lines += "| [$($f['id'])]($($t.FileName)) | $($f['priority']) | $($f['status']) | $($f['area']) | $($f['title'])$mark |"
        }
        $lines += ''
    }

    # 3. Waiting for verification
    $lines += '## 🧪 Ждут проверки (fixed)'
    $lines += ''
    $lines += 'Агент считает, что починил; вы это не проверяли. Проверить надо одно: проблема действительно ушла?'
    $lines += ''
    $lines += (Format-Checklist)

    # 4. Decisions
    $lines += '## 📐 Решения (decision)'
    $lines += ''
    $decisions = @()
    foreach ($t in $valid) { if ($t.Fields['type'] -eq 'decision') { $decisions += $t } }
    if ($decisions.Count -eq 0) {
        $lines += '_Пока нет._'
        $lines += ''
    }
    else {
        $lines += '| ID | Принято | Статус | Область | Решение |'
        $lines += '|---|---|---|---|---|'
        foreach ($t in ($decisions | Sort-Object { $_.Fields['created'] } -Descending)) {
            $f = $t.Fields
            $lines += "| [$($f['id'])]($($t.FileName)) | $($f['created']) | $($f['status']) | $($f['area']) | $($f['title']) |"
        }
        $lines += ''
    }

    # 5. Recently closed
    $lines += '## ✅ Недавно закрытые (14 дней)'
    $lines += ''
    $cutoff = (Get-Date).AddDays(-14)
    $recent = @()
    foreach ($t in $valid) {
        $s = $t.Fields['status']
        if ($s -ne 'verified' -and $s -ne 'closed' -and $s -ne 'wontfix' -and $s -ne 'cannot-reproduce') { continue }
        $parsed = [datetime]::MinValue
        if ([datetime]::TryParseExact($t.Fields['updated'], 'yyyy-MM-dd', $null, [Globalization.DateTimeStyles]::None, [ref]$parsed)) {
            if ($parsed -ge $cutoff) { $recent += $t }
        }
    }
    if ($recent.Count -eq 0) {
        $lines += '_Пусто._'
        $lines += ''
    }
    else {
        $lines += '| ID | Обновлён | Статус | Заголовок |'
        $lines += '|---|---|---|---|'
        foreach ($t in ($recent | Sort-Object { $_.Fields['updated'] } -Descending)) {
            $f = $t.Fields
            $lines += "| [$($f['id'])]($($t.FileName)) | $($f['updated']) | $($f['status']) | $($f['title']) |"
        }
        $lines += ''
    }

    $path = Join-Path (Get-TicketsDir) 'BOARD.md'
    Write-TextFile $path (($lines -join "`n").TrimEnd() + "`n")
    Write-Host "board written: $path ($($valid.Count) tickets)" -ForegroundColor Green
    Invoke-Html   # keep the browser view in sync with the markdown board
}

# ------------------------------------------------------------------ html view --

function Invoke-Html {
    $tickets = @(Get-AllTickets)
    $chunks = @()
    foreach ($t in $tickets) {
        $f = $t.Fields
        if ($f.Count -eq 0 -or -not $f.Contains('status')) { continue }
        $obj = [ordered]@{
            id       = $f['id']
            title    = $f['title']
            type     = $f['type']
            status   = $f['status']
            priority = $f['priority']
            area     = $f['area']
            source   = $f['source']
            verify   = $(if ($f.Contains('verify-in')) { $f['verify-in'] } else { '' })
            created  = $f['created']
            updated  = $f['updated']
            links    = $(if ($f.Contains('links')) { $f['links'] } else { '' })
            commits  = $(if ($f.Contains('commits')) { $f['commits'] } else { '' })
            labels   = $(if ($f.Contains('labels')) { $f['labels'] } else { '' })
            file     = $t.FileName
            body     = $t.Body.Trim()
        }
        # per-object conversion: ConvertTo-Json on a 1-element array yields an object, not an array
        $chunks += ([pscustomobject]$obj | ConvertTo-Json -Depth 3 -Compress)
    }
    $json = '[' + ($chunks -join ',') + ']'

    $html = $HtmlTemplate.Replace('/*__DATA__*/null', $json).Replace('__GENERATED__', (Today))
    $path = Join-Path (Get-TicketsDir) 'board.html'
    Write-TextFile $path $html
    Write-Host "html written: $path ($($chunks.Count) tickets)" -ForegroundColor Green
}

$HtmlTemplate = @'
<!doctype html>
<html lang="ru">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Тикеты XMB</title>
<style>
:root {
  --bg: #ffffff; --fg: #1a1a1a; --muted: #6b7280; --line: #e5e7eb; --card: #f9fafb;
  --accent: #2563eb; --p0: #dc2626; --p1: #ea580c; --p2: #2563eb; --p3: #6b7280;
  --ok: #16a34a; --warn: #d97706;
}
@media (prefers-color-scheme: dark) {
  :root {
    --bg: #0f1115; --fg: #e6e6e6; --muted: #9aa0aa; --line: #262b33; --card: #171a20;
    --accent: #60a5fa; --p0: #f87171; --p1: #fb923c; --p2: #60a5fa; --p3: #9aa0aa;
    --ok: #4ade80; --warn: #fbbf24;
  }
}
* { box-sizing: border-box; }
body {
  margin: 0; background: var(--bg); color: var(--fg);
  font: 14px/1.5 -apple-system, "Segoe UI", Roboto, sans-serif;
}
header { padding: 16px 20px 0; border-bottom: 1px solid var(--line); }
h1 { font-size: 18px; margin: 0 0 2px; }
.sub { color: var(--muted); font-size: 12px; margin-bottom: 12px; }
.tabs { display: flex; gap: 4px; }
.tab {
  padding: 8px 14px; border: 1px solid var(--line); border-bottom: none; cursor: pointer;
  background: var(--card); color: var(--muted); border-radius: 6px 6px 0 0; font-size: 13px;
}
.tab.on { background: var(--bg); color: var(--fg); font-weight: 600; box-shadow: inset 0 2px 0 var(--accent); }
.tab .n { color: var(--muted); font-weight: 400; }
main { padding: 16px 20px 60px; max-width: 1200px; }
.bar { display: flex; flex-wrap: wrap; gap: 8px; align-items: center; margin-bottom: 12px; }
input[type=text], textarea, select {
  background: var(--card); color: var(--fg); border: 1px solid var(--line);
  border-radius: 6px; padding: 6px 9px; font: inherit;
}
input[type=text]:focus, textarea:focus, select:focus { outline: 2px solid var(--accent); outline-offset: -1px; }
#q { min-width: 260px; flex: 1; }
.chip {
  border: 1px solid var(--line); background: var(--card); color: var(--muted);
  border-radius: 999px; padding: 3px 10px; cursor: pointer; font-size: 12px;
}
.chip.on { background: var(--accent); border-color: var(--accent); color: #fff; }
button {
  background: var(--card); color: var(--fg); border: 1px solid var(--line);
  border-radius: 6px; padding: 6px 12px; cursor: pointer; font: inherit;
}
button:hover { border-color: var(--accent); }
button.primary { background: var(--accent); border-color: var(--accent); color: #fff; }
.row {
  display: grid; grid-template-columns: 92px 28px 110px 88px 1fr; gap: 10px; align-items: baseline;
  padding: 7px 8px; border-bottom: 1px solid var(--line); cursor: pointer;
}
.row:hover { background: var(--card); }
.row .id { font-family: ui-monospace, Consolas, monospace; font-size: 12px; color: var(--accent); }
.p { font-weight: 700; }
.P0 { color: var(--p0); } .P1 { color: var(--p1); } .P2 { color: var(--p2); } .P3 { color: var(--p3); }
.st { font-size: 12px; color: var(--muted); }
.st.fixed { color: var(--warn); } .st.open { color: var(--fg); }
.st.verified, .st.closed, .st.accepted { color: var(--ok); }
.area { font-size: 12px; color: var(--muted); }
.body { padding: 4px 8px 18px 100px; border-bottom: 1px solid var(--line); background: var(--card); }
.body h3 { font-size: 13px; text-transform: uppercase; letter-spacing: .04em; color: var(--muted); margin: 14px 0 4px; }
.body code { background: var(--bg); padding: 1px 5px; border-radius: 4px; font-size: 12.5px; }
.body ul, .body ol { margin: 4px 0; padding-left: 22px; }
.meta { color: var(--muted); font-size: 12px; margin-top: 10px; }
.hint { color: var(--muted); font-size: 13px; margin: 6px 0 16px; }
.group { margin: 22px 0 8px; font-size: 13px; font-weight: 600; }
.pt { border: 1px solid var(--line); border-radius: 8px; padding: 10px 12px; margin-bottom: 8px; background: var(--card); }
.pt-head { display: flex; gap: 10px; align-items: baseline; flex-wrap: wrap; }
.pt-actions { display: flex; gap: 6px; margin: 8px 0 0; flex-wrap: wrap; align-items: center; }
.pt.ok { border-color: var(--ok); }
.pt.fail { border-color: var(--p0); }
button.v.on-ok { background: var(--ok); border-color: var(--ok); color: #08130b; font-weight: 600; }
button.v.on-fail { background: var(--p0); border-color: var(--p0); color: #fff; font-weight: 600; }
.note { flex: 1; min-width: 220px; }
.fb { border: 1px solid var(--line); border-radius: 8px; padding: 12px; background: var(--card); margin-bottom: 14px; }
.fb .grid { display: flex; gap: 8px; flex-wrap: wrap; margin-bottom: 8px; }
.fb textarea { width: 100%; min-height: 70px; }
.fbitem { border-bottom: 1px solid var(--line); padding: 8px 0; display: flex; gap: 10px; align-items: baseline; }
.fbitem .x { color: var(--p0); cursor: pointer; }
textarea.out { width: 100%; min-height: 320px; font-family: ui-monospace, Consolas, monospace; font-size: 12.5px; }
.empty { color: var(--muted); padding: 20px 0; }
.count { color: var(--muted); font-size: 12px; margin: 10px 0; }
.badge { font-size: 11px; border: 1px solid var(--line); border-radius: 4px; padding: 0 5px; color: var(--muted); }
</style>
</head>
<body>
<header>
  <h1>Тикеты XMB</h1>
  <div class="sub">Сгенерировано __GENERATED__ из <code>tickets/</code> · страница только читает файлы; изменения выдаются командами во вкладке «Выгрузка»</div>
  <div class="tabs">
    <div class="tab on" data-v="board">Доска <span class="n" id="n-board"></span></div>
    <div class="tab" data-v="review">Ревью <span class="n" id="n-pt"></span></div>
    <div class="tab" data-v="feedback">Новый фидбек <span class="n" id="n-fb"></span></div>
    <div class="tab" data-v="export">Выгрузка для агента</div>
  </div>
</header>
<main>
  <section id="v-board">
    <div class="bar">
      <input type="text" id="q" placeholder="поиск по заголовку, тексту, id...">
      <span id="f-prio"></span><span id="f-status"></span>
    </div>
    <div class="bar"><span id="f-area"></span></div>
    <div class="count" id="cnt"></div>
    <div id="list"></div>
  </section>

  <section id="v-review" hidden>
    <div class="hint">Тикеты в статусе <b>fixed</b>: агент считает, что починил, но вы это не проверяли. Вопрос по каждому — <b>работает ли теперь правильно</b>. «Ушёл» = проблемы больше нет, тикет закрывается (<code>verified</code>). «Остался» = баг всё ещё воспроизводится, тикет возвращается в работу (<code>open</code>). Заметка попадёт в историю тикета — для «остался» она особенно важна.</div>
    <div class="bar"><button id="pt-reset">Сбросить отметки</button></div>
    <div id="ptlist"></div>
  </section>

  <section id="v-feedback" hidden>
    <div class="hint">Здесь копится НОВЫЙ фидбек с ревью — то, на что тикетов ещё нет. Агент заведёт по тикету на пункт.</div>
    <div class="fb">
      <div class="grid">
        <input type="text" id="fb-title" placeholder="Коротко: что не так" style="flex:1;min-width:300px">
        <select id="fb-type"><option value="bug">баг</option><option value="feature">фича</option><option value="tech">тех. долг</option><option value="question">вопрос</option></select>
        <select id="fb-prio"><option>P0</option><option>P1</option><option selected>P2</option><option>P3</option></select>
        <select id="fb-area"></select>
      </div>
      <textarea id="fb-desc" placeholder="Подробности: что делали, что увидели, что ожидали. Чем конкретнее, тем меньше вопросов от агента."></textarea>
      <div class="bar" style="margin-top:8px"><button class="primary" id="fb-add">Добавить пункт</button></div>
    </div>
    <div id="fblist"></div>
  </section>

  <section id="v-export" hidden>
    <div class="hint">Первый блок — вставьте агенту в чат. Второй — команды, которые агент выполнит сам (или выполните вручную в PowerShell из корня проекта).</div>
    <div class="bar">
      <button class="primary" id="save1">Сохранить фидбек в файл</button>
      <button id="copy1">Скопировать отчёт для агента</button>
      <button id="copy2">Скопировать команды</button>
      <button id="clear">Очистить сессию</button>
    </div>
    <h3>Отчёт</h3>
    <textarea class="out" id="out1" readonly></textarea>
    <h3>Команды CLI</h3>
    <textarea class="out" id="out2" readonly style="min-height:160px"></textarea>
  </section>
</main>

<script>
const DATA = /*__DATA__*/null;
const AREAS = ['ui','player','audio','library','net','power','display','input','firmware','tools','docs','infra'];
const KEY = 'xmb-tickets-session';
const S = Object.assign({ verdicts: {}, notes: {}, feedback: [] }, JSON.parse(localStorage.getItem(KEY) || '{}'));
const save = () => localStorage.setItem(KEY, JSON.stringify(S));
const esc = s => String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');
const byId = id => DATA.find(t => t.id === id);
const today = () => new Date().toISOString().slice(0,10);

/* markdown-lite: headings, lists, code, bold, ticket links */
function md(src) {
  const inline = s => esc(s)
    .replace(/`([^`]+)`/g, '<code>$1</code>')
    .replace(/\*\*([^*]+)\*\*/g, '<b>$1</b>')
    .replace(/\[(XMB-\d{4})\]\([^)]*\)/g, '<a href="#" data-goto="$1">$1</a>')
    .replace(/\[([^\]]+)\]\(([^)]+)\)/g, '<a href="$2">$1</a>');
  let out = '', list = null;
  const close = () => { if (list) { out += '</' + list + '>'; list = null; } };
  for (const raw of src.split('\n')) {
    const line = raw.trimEnd();
    let m;
    if (!line.trim()) { close(); continue; }
    if ((m = line.match(/^##\s+(.*)$/))) { close(); out += '<h3>' + inline(m[1]) + '</h3>'; }
    else if ((m = line.match(/^[-*]\s+(.*)$/))) {
      if (list !== 'ul') { close(); out += '<ul>'; list = 'ul'; }
      out += '<li>' + inline(m[1]) + '</li>';
    }
    else if ((m = line.match(/^(\d+)\.\s+(.*)$/))) {
      if (list !== 'ol') { close(); out += '<ol>'; list = 'ol'; }
      out += '<li>' + inline(m[2]) + '</li>';
    }
    else { close(); out += '<p>' + inline(line) + '</p>'; }
  }
  close();
  return out;
}

/* ---------------------------------------------------------------- board tab */
const F = { q: '', prio: new Set(), status: new Set(), area: new Set() };
const STATUSES = ['open','in-progress','fixed','verified','cannot-reproduce','closed','wontfix','accepted','superseded'];

function chips(host, values, set, render) {
  host.innerHTML = '';
  values.forEach(v => {
    const b = document.createElement('span');
    b.className = 'chip' + (set.has(v) ? ' on' : '');
    b.textContent = v;
    b.onclick = () => { set.has(v) ? set.delete(v) : set.add(v); chips(host, values, set, render); render(); };
    host.appendChild(b);
  });
}

function matches(t) {
  if (F.prio.size && !F.prio.has(t.priority)) return false;
  if (F.status.size && !F.status.has(t.status)) return false;
  if (F.area.size && !F.area.has(t.area)) return false;
  if (F.q) {
    const hay = (t.id + ' ' + t.title + ' ' + t.body + ' ' + t.labels).toLowerCase();
    if (!F.q.toLowerCase().split(/\s+/).every(w => hay.includes(w))) return false;
  }
  return true;
}

const PORD = { P0: 0, P1: 1, P2: 2, P3: 3 };
const SORD = { open: 0, 'in-progress': 0, fixed: 1, accepted: 2 };

function renderBoard() {
  const list = document.getElementById('list');
  const rows = DATA.filter(matches).sort((a, b) =>
    (SORD[a.status] ?? 3) - (SORD[b.status] ?? 3) || PORD[a.priority] - PORD[b.priority] || a.id.localeCompare(b.id));
  document.getElementById('cnt').textContent = rows.length + ' из ' + DATA.length;
  list.innerHTML = '';
  if (!rows.length) { list.innerHTML = '<div class="empty">Ничего не найдено.</div>'; return; }
  rows.forEach(t => {
    const r = document.createElement('div');
    r.className = 'row'; r.id = 'row-' + t.id;
    r.innerHTML = '<span class="id">' + t.id + '</span>'
      + '<span class="p ' + t.priority + '">' + t.priority + '</span>'
      + '<span class="st ' + t.status + '">' + t.status + '</span>'
      + '<span class="area">' + t.area + '</span>'
      + '<span>' + esc(t.title) + (t.labels.includes('watch') ? ' 👀' : '') + '</span>';
    const d = document.createElement('div');
    d.className = 'body'; d.hidden = true;
    r.onclick = () => {
      if (!d.dataset.done) {
        d.innerHTML = md(t.body) + '<div class="meta">тип: ' + t.type + ' · источник: ' + t.source
          + ' · проверка: ' + (t.verify || '—') + ' · создан: ' + t.created + ' · обновлён: ' + t.updated
          + (t.links ? ' · связи: ' + t.links : '') + (t.commits ? ' · коммиты: ' + t.commits : '')
          + '<br>файл: <code>tickets/' + t.file + '</code></div>';
        d.dataset.done = '1';
      }
      d.hidden = !d.hidden;
    };
    list.appendChild(r); list.appendChild(d);
  });
}

/* ------------------------------------------------------------- review tab */
const VERIFY_GROUPS = [
  ['review', 'Проверить НА ПЛЕЕРЕ'],
  ['build', 'Проверить БИЛДОМ'],
  ['test', 'Проверить ТЕСТОМ'],
  ['', 'Прочее']
];

function renderReview() {
  const host = document.getElementById('ptlist');
  const fixed = DATA.filter(t => t.status === 'fixed');
  host.innerHTML = '';
  if (!fixed.length) { host.innerHTML = '<div class="empty">Нечего проверять: тикетов в статусе fixed нет.</div>'; return; }
  VERIFY_GROUPS.forEach(([key, caption]) => {
    const items = fixed.filter(t => (t.verify || '') === key || (key === '' && !['review','build','test'].includes(t.verify)));
    if (!items.length) return;
    const h = document.createElement('div');
    h.className = 'group'; h.textContent = caption + ' (' + items.length + ')';
    host.appendChild(h);
    items.sort((a, b) => (PORD[a.priority] - PORD[b.priority])
        || String(b.updated || '').localeCompare(String(a.updated || ''))).forEach(t => {
      const v = S.verdicts[t.id] || '';
      const card = document.createElement('div');
      card.className = 'pt' + (v ? ' ' + v : '');
      /* WHY a badge: the checklist is 20+ items long, so a newly fixed ticket is
         invisible among the ones already checked last time — the user looked for
         new items and did not find them. 'New' means: changed after the last
         report was saved from this UI. */
      const fresh = S.lastSaved && t.updated && t.updated >= S.lastSaved;
      card.innerHTML = '<div class="pt-head"><span class="id" style="font-family:ui-monospace,Consolas,monospace">' + t.id + '</span>'
        + '<span class="p ' + t.priority + '">' + t.priority + '</span>'
        + '<span class="badge">' + t.area + '</span>'
        + (fresh ? '<span class="badge" style="background:#1f6f3f;color:#eaffef">новое</span>' : '')
        + '<b style="flex:1">' + esc(t.title) + '</b>'
        + '<span class="badge" title="когда тикет менялся последний раз">' + (t.updated || '') + '</span>'
        + '<button class="det">детали</button></div>';
      const det = document.createElement('div');
      det.hidden = true; det.style.marginTop = '8px';
      det.innerHTML = md(t.body);
      card.querySelector('.det').onclick = () => { det.hidden = !det.hidden; };
      const act = document.createElement('div');
      act.className = 'pt-actions';
      const mk = (val, label, cls) => {
        const b = document.createElement('button');
        b.className = 'v' + (v === val ? ' on-' + cls : '');
        b.textContent = label;
        b.onclick = () => {
          S.verdicts[t.id] = (S.verdicts[t.id] === val) ? '' : val;
          save(); renderReview(); renderCounts();
        };
        return b;
      };
      const isBug = t.type === 'bug';
      act.appendChild(mk('ok', isBug ? '✓ баг ушёл' : '✓ работает как надо', 'ok'));
      act.appendChild(mk('fail', isBug ? '✗ баг остался' : '✗ работает не так', 'fail'));
      const note = document.createElement('input');
      note.type = 'text'; note.className = 'note';
      note.placeholder = v === 'fail' ? 'что именно осталось не так (важно для агента)' : 'заметка (попадёт в историю тикета)';
      note.value = S.notes[t.id] || '';
      note.oninput = () => { S.notes[t.id] = note.value; save(); renderExport(); };
      act.appendChild(note);
      card.appendChild(act); card.appendChild(det);
      host.appendChild(card);
    });
  });
}

/* ------------------------------------------------------------- feedback tab */
function renderFeedback() {
  const host = document.getElementById('fblist');
  host.innerHTML = '';
  if (!S.feedback.length) { host.innerHTML = '<div class="empty">Пунктов пока нет.</div>'; return; }
  S.feedback.forEach((f, i) => {
    const d = document.createElement('div');
    d.className = 'fbitem';
    d.innerHTML = '<span class="p ' + f.prio + '">' + f.prio + '</span><span class="badge">' + f.area + '</span>'
      + '<span class="badge">' + f.type + '</span><span style="flex:1"><b>' + esc(f.title) + '</b>'
      + (f.desc ? '<br><span style="color:var(--muted)">' + esc(f.desc) + '</span>' : '') + '</span>'
      + '<span class="x" title="удалить">✕</span>';
    d.querySelector('.x').onclick = () => { S.feedback.splice(i, 1); save(); renderFeedback(); renderCounts(); renderExport(); };
    host.appendChild(d);
  });
}

/* --------------------------------------------------------------- export tab */
function renderExport() {
  const ok = DATA.filter(t => S.verdicts[t.id] === 'ok');
  const bad = DATA.filter(t => S.verdicts[t.id] === 'fail');
  const d = today();
  let r = '## Итоги ревью ' + d + '\n';
  if (ok.length) {
    r += '\n### Фикс работает, проблемы больше нет — ставь verified\n';
    ok.forEach(t => { r += '- ' + t.id + ' — ' + t.title + (S.notes[t.id] ? ' — ' + S.notes[t.id] : '') + '\n'; });
  }
  if (bad.length) {
    r += '\n### Фикс НЕ помог, проблема осталась — возвращай в open\n';
    bad.forEach(t => { r += '- ' + t.id + ' — ' + t.title + (S.notes[t.id] ? ' — ' + S.notes[t.id] : ' — подробностей нет') + '\n'; });
  }
  if (S.feedback.length) {
    r += '\n### Новый фидбек — заведи тикеты\n';
    S.feedback.forEach((f, i) => {
      r += (i + 1) + '. [' + f.prio + '/' + f.area + '/' + f.type + '] ' + f.title + '\n';
      if (f.desc) r += '   ' + f.desc.replace(/\n/g, '\n   ') + '\n';
    });
  }
  if (!ok.length && !bad.length && !S.feedback.length) r += '\n_Пока ничего не отмечено._\n';
  document.getElementById('out1').value = r;

  let c = '';
  ok.forEach(t => {
    c += 'powershell -File scripts/tickets.ps1 set -Id ' + t.id + ' -Status verified -Note "ревью ' + d + ': фикс работает'
      + (S.notes[t.id] ? ' — ' + S.notes[t.id].replace(/"/g, "'") : '') + '"\n';
  });
  bad.forEach(t => {
    c += 'powershell -File scripts/tickets.ps1 set -Id ' + t.id + ' -Status open -Note "ревью ' + d + ': фикс НЕ помог, проблема осталась — '
      + (S.notes[t.id] || 'подробностей нет').replace(/"/g, "'") + '"\n';
  });
  if (c) c += 'powershell -File scripts/tickets.ps1 board\n';
  document.getElementById('out2').value = c || '# отметок нет';
}

function renderCounts() {
  document.getElementById('n-board').textContent = DATA.length;
  const fixed = DATA.filter(t => t.status === 'fixed').length;
  const done = Object.values(S.verdicts).filter(Boolean).length;
  document.getElementById('n-pt').textContent = done ? done + '/' + fixed : fixed;
  document.getElementById('n-fb').textContent = S.feedback.length || '';
  renderExport();
}

/* ------------------------------------------------------------------- wiring */
document.querySelectorAll('.tab').forEach(tab => {
  tab.onclick = () => {
    document.querySelectorAll('.tab').forEach(x => x.classList.toggle('on', x === tab));
    ['board','review','feedback','export'].forEach(v =>
      document.getElementById('v-' + v).hidden = (v !== tab.dataset.v));
    if (tab.dataset.v === 'export') renderExport();
  };
});
document.getElementById('q').oninput = e => { F.q = e.target.value; renderBoard(); };
chips(document.getElementById('f-prio'), ['P0','P1','P2','P3'], F.prio, renderBoard);
chips(document.getElementById('f-status'), STATUSES, F.status, renderBoard);
chips(document.getElementById('f-area'), AREAS, F.area, renderBoard);
AREAS.forEach(a => { const o = document.createElement('option'); o.textContent = a; document.getElementById('fb-area').appendChild(o); });
document.getElementById('fb-add').onclick = () => {
  const title = document.getElementById('fb-title').value.trim();
  if (!title) { document.getElementById('fb-title').focus(); return; }
  S.feedback.push({
    title, desc: document.getElementById('fb-desc').value.trim(),
    prio: document.getElementById('fb-prio').value,
    area: document.getElementById('fb-area').value,
    type: document.getElementById('fb-type').value
  });
  save();
  document.getElementById('fb-title').value = ''; document.getElementById('fb-desc').value = '';
  renderFeedback(); renderCounts();
};
document.getElementById('pt-reset').onclick = () => {
  if (!confirm('Сбросить все отметки проверки?')) return;
  S.verdicts = {}; S.notes = {}; save(); renderReview(); renderCounts();
};
document.getElementById('clear').onclick = () => {
  if (!confirm('Очистить отметки И собранный фидбек?')) return;
  S.verdicts = {}; S.notes = {}; S.feedback = []; save();
  renderReview(); renderFeedback(); renderCounts();
};
const copy = (id, btn) => {
  const el = document.getElementById(id);
  el.select(); document.execCommand('copy');
  const old = btn.textContent; btn.textContent = 'скопировано ✓';
  setTimeout(() => { btn.textContent = old; }, 1200);
};
/* Save the feedback as a file the agent can apply directly, so nothing has to be
   copied by hand (user request 2026-07-26). A browser cannot write into the repo,
   so this either asks for a location once (File System Access API, pick
   tickets/feedback.json) or falls back to a normal download. */
document.getElementById('save1').onclick = async e => {
  const payload = JSON.stringify({
    generated: new Date().toISOString(),
    verdicts: S.verdicts,
    notes: S.notes,
    feedback: S.feedback
  }, null, 2);
  const btn = e.target;
  const done = where => {
    S.lastSaved = today(); save(); renderReview();
    btn.textContent = 'Сохранено: ' + where; setTimeout(() => btn.textContent = 'Сохранить фидбек в файл', 2500);
  };
  try {
    if (window.showSaveFilePicker) {
      const handle = await window.showSaveFilePicker({
        suggestedName: 'feedback.json',
        types: [{ description: 'JSON', accept: { 'application/json': ['.json'] } }]
      });
      const writable = await handle.createWritable();
      await writable.write(payload);
      await writable.close();
      done(handle.name);
      return;
    }
  } catch (err) {
    if (err && err.name === 'AbortError') return; // user cancelled: say nothing
  }
  const blob = new Blob([payload], { type: 'application/json' });
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = 'feedback.json';
  a.click();
  URL.revokeObjectURL(a.href);
  done('Downloads');
};

document.getElementById('copy1').onclick = e => copy('out1', e.target);
document.getElementById('copy2').onclick = e => copy('out2', e.target);
document.addEventListener('click', e => {
  const id = e.target.dataset && e.target.dataset.goto;
  if (!id) return;
  e.preventDefault();
  document.querySelector('.tab[data-v=board]').click();
  F.q = id; document.getElementById('q').value = id; renderBoard();
  const row = document.getElementById('row-' + id);
  if (row) { row.click(); row.scrollIntoView({ block: 'center' }); }
});

renderBoard(); renderReview(); renderFeedback(); renderCounts();
</script>
</body>
</html>
'@

function Invoke-Help {
    Write-Host @'
tickets.ps1 - file-based tickets for XMB

  new       -Type <bug|feature|tech|decision|question> -Title "..." [-Priority P0..P3]
            -Area <ui|player|audio|library|net|power|display|input|firmware|tools|docs|infra>
            [-Source agent|user|review-YYYY-MM-DD] [-VerifyIn review|test|build|none]
            [-Status ...] [-Slug ...] [-Links ...] [-Labels ...] [-Note "первая строка истории"]

  set       -Id XMB-0042 [-Status ...] [-Priority ...] [-Area ...] [-VerifyIn ...]
            [-Title "..."] [-Commits abc1234] [-Links XMB-0001] [-Labels watch]
            [-Note "что произошло"] [-Force]

  list      [-Status ...] [-Type ...] [-Area ...] [-Priority ...]   (без фильтров: активные)
  checklist                                                          что проверить (fixed)
  board                                                              перегенерировать BOARD.md + board.html
  html                                                               только tickets/board.html (UI в браузере)
  validate                                                           проверить все тикеты
  apply     [-Path tickets/feedback.json]                            применить фидбек из board.html
                                                                     (кнопка «Сохранить фидбек в файл»)

Правила и процессы: tickets/README.md
'@
}

# --------------------------------------------------------------------- main --

switch ($Command.ToLowerInvariant()) {
    'new' { Invoke-New }
    'set' { Invoke-Set }
    'list' { Invoke-List }
    'checklist' { Invoke-Checklist }
    'board' { Invoke-Board }
    'html' { Invoke-Html }
    'validate' { Invoke-Validate }
    'apply' { Invoke-Apply }
    'help' { Invoke-Help }
    default {
        Write-Host "unknown command '$Command'"
        Invoke-Help
        exit 1
    }
}
