<#
    Installs MilkDrop preset packs and the shared texture pack for the OpenDJ
    visualiser. The PowerShell twin of scripts/get-presets.sh.

      pwsh scripts/get-presets.ps1                 install into %APPDATA%\OpenDJ
      pwsh scripts/get-presets.ps1 -List           show the packs and their sizes
      pwsh scripts/get-presets.ps1 -Prefix <path>  install somewhere else

    The presets are not part of this repository. They were released over two
    decades by many authors, almost none under any stated licence, and the
    projectM team keeps them in repositories of their own on that footing. This
    fetches them at your request rather than shipping them, which also keeps a
    hundred megabytes of somebody else's art out of a DJ application's history.

    A note on Butterchurn, since it is the obvious thing to ask for: its preset
    packs are the same MilkDrop presets, but converted, with the equations
    turned into JavaScript and the shaders into GLSL. projectM reads the
    original MilkDrop expression language and HLSL, so those files cannot be
    used here, and converting them back would mean reverse-translating two
    languages. The packs below are where Butterchurn's presets came from, and
    they hold about six times as many.

    %APPDATA%\OpenDJ is where the settings and the library already live, and it
    is the one place a user can write without being an administrator. For an
    installed copy, point -Prefix at the share\opendj folder beside OpenDJ.exe;
    there is no /usr/local on Windows to have a --system switch for.
#>
[CmdletBinding()]
param(
    [switch]$List,
    [string]$Prefix = (Join-Path $env:APPDATA 'OpenDJ')
)

$ErrorActionPreference = 'Stop'

# name, repository, what it is
$packs = @(
    @{ Name = 'cream-of-the-crop';  Repository = 'presets-cream-of-the-crop';  Description = '9795 presets, the large curated collection' }
    @{ Name = 'projectm-classic';   Repository = 'presets-projectm-classic';   Description = '4188 presets, the set projectM has shipped for years' }
    @{ Name = 'milkdrop-original';  Repository = 'presets-milkdrop-original';  Description = '552 presets, the ones MilkDrop itself came with' }
    @{ Name = 'en-d';               Repository = 'presets-en-d';               Description = '40 presets by en-d' }
)

$texturePack = 'presets-milkdrop-texture-pack'

if ($List) {
    '{0,-20} {1}' -f 'PACK', 'WHAT IT IS'
    foreach ($pack in $packs) { '{0,-20} {1}' -f $pack.Name, $pack.Description }
    '{0,-20} {1}' -f 'textures', '67 images the presets above load by name'
    ''
    "Installs into: $Prefix"
    exit 0
}

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    throw 'git is needed to fetch the presets.'
}

New-Item -ItemType Directory -Force (Join-Path $Prefix 'presets') | Out-Null
New-Item -ItemType Directory -Force (Join-Path $Prefix 'textures') | Out-Null

function Get-Pack {
    param([string]$Repository, [string]$Destination)

    # A shallow clone, then the history thrown away: what is wanted is the
    # files, and keeping the .git would roughly double what this costs on disk
    # for no benefit to anybody.
    #
    # Cloned into a short path under TEMP and moved afterwards, not cloned
    # straight into place. Cream of the Crop has preset names long enough that
    # a full path under %APPDATA% passes the 260 characters Windows stops at,
    # and Git for Windows has core.longpaths off unless told otherwise. Both
    # halves of that are handled here: the switch below, and somewhere short to
    # land first.
    $work = Join-Path $env:TEMP ("odp-" + [System.IO.Path]::GetRandomFileName())
    New-Item -ItemType Directory -Force $work | Out-Null

    try {
        Write-Host "Fetching $Repository ..."
        & git -c core.longpaths=true clone --depth 1 --quiet `
              "https://github.com/projectM-visualizer/$Repository.git" (Join-Path $work 'pack')

        if ($LASTEXITCODE -ne 0) { throw "Could not clone $Repository." }

        Remove-Item -Recurse -Force (Join-Path $work 'pack\.git') -ErrorAction SilentlyContinue

        if (Test-Path $Destination) { Remove-Item -Recurse -Force $Destination }
        New-Item -ItemType Directory -Force $Destination | Out-Null

        Move-Item -Path (Join-Path $work 'pack\*') -Destination $Destination -Force
    }
    finally {
        Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
    }
}

foreach ($pack in $packs) {
    Get-Pack -Repository $pack.Repository -Destination (Join-Path $Prefix "presets\$($pack.Name)")
}

# The textures go in one flat directory rather than a tree: projectM is given
# directories to look in, not trees to walk, and the pack arrives with its
# images spread over subfolders.
$textureFolder = Join-Path $Prefix 'textures\milkdrop'
Get-Pack -Repository $texturePack -Destination $textureFolder

Get-ChildItem -Path $textureFolder -Recurse -File `
              -Include *.jpg, *.jpeg, *.png, *.dds, *.tga -ErrorAction SilentlyContinue |
    Where-Object { $_.DirectoryName -ne (Resolve-Path $textureFolder).Path } |
    ForEach-Object {
        $target = Join-Path $textureFolder $_.Name

        # The first of a name wins, the way the shell script's mv -n does.
        if (-not (Test-Path $target)) { Move-Item -Path $_.FullName -Destination $target }
    }

$presetCount = (Get-ChildItem -Path (Join-Path $Prefix 'presets') -Recurse -Filter *.milk `
                              -ErrorAction SilentlyContinue | Measure-Object).Count
$textureCount = (Get-ChildItem -Path (Join-Path $Prefix 'textures') -Recurse -File `
                               -ErrorAction SilentlyContinue | Measure-Object).Count

''
"Installed $presetCount presets and $textureCount textures into $Prefix"
'Start OpenDJ and press Visuals; click the picture for the next preset.'
