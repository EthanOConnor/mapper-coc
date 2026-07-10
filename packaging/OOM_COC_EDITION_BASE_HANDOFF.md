# OOM — COC Edition: Base handoff checklist

Status: pre-release checklist; completing or merging the associated code
changes does not by itself approve distribution.

**OOM — COC Edition: Base** is a conservative class-preview build of
OpenOrienteering Mapper. It is distinct from both the upstream project and the
`full-speed-ahead` experimental line. The internal executable and settings
identity remain Mapper so existing maps and preferences continue to work.

## Supported release scope

| Platform | Class-preview status |
|---|---|
| Windows x64 | Supported after every gate below passes |
| macOS | Not distributed for this preview |
| Linux packages | Not distributed for this preview |
| Android | Not distributed for this preview |

The supported online-imagery contract is deliberately narrow:

- top-origin, 256 px Web Mercator XYZ tiles through the configured level; or
- standard cached, 256 px Web Mercator ArcGIS MapServer tiles.

Bottom-origin TMS, arbitrary ArcGIS cache grids, authentication, provider
metadata discovery, and attribution UI are not implemented. The exact source
intended for the class must be validated manually.

## Automated candidate gates

- [ ] Linux, macOS, and Windows build/test jobs pass for the exact commit.
- [ ] The packaged Windows GDAL/curl TLS check passes using Schannel.
- [ ] The portable Mapper tree stays open for the startup-smoke interval.
- [ ] The NSIS installer installs silently into a clean temporary directory.
- [ ] The installed Mapper tree contains Qt, GDAL, and PROJ runtime data.
- [ ] The installed Mapper executable stays open for the startup-smoke
      interval.
- [ ] The preview is configured with `Mapper_PACKAGE_FILE_ASSOCIATIONS=OFF`.
- [ ] The uploaded release manifest records artifact names, byte sizes,
      SHA-256 digests, signing status, branch, commit, and Actions run URL.

## Internal-alpha packaging boundary

This handoff uses the project's existing packaging and notice outputs. It does
not add a custom package-owner or license-completeness system for the Windows
runtime. Before public or broader external distribution, perform a separate
dependency and notice review. For this internal alpha, record any known notice
gaps during the candidate review rather than treating CI as a compliance audit.

## Exact Windows candidate

Complete this section from the generated release manifest after the final
candidate run. Do not reuse values from an earlier run.

- Product: OOM — COC Edition: Base
- Version:
- Branch:
- Full commit SHA:
- Actions run URL:
- Installer filename:
- Installer byte size:
- Installer SHA-256:
- Authenticode status:
- Portable ZIP filename and SHA-256:
- Exact imagery source URL:

PowerShell verification command:

```powershell
(Get-FileHash -Algorithm SHA256 .\<installer-file>).Hash
```

The result must exactly match the published manifest before the installer is
run.

## Clean-machine acceptance

- [ ] If `0.9.7-COC.3` is installed, uninstall it with its own uninstaller
      before installing `0.9.7-COC.4`; record the existing official Mapper
      file associations first. The COC.4 uninstaller intentionally does not
      invoke the older shared-association cleanup code.
- [ ] Use the exact installer named above on a clean Windows machine.
- [ ] Record the expected SmartScreen/unknown-publisher flow if unsigned.
- [ ] Confirm installation and first startup.
- [ ] Confirm an existing official Mapper installation still opens before and
      after preview installation.
- [ ] Add the exact intended imagery URL.
- [ ] Pan and zoom through fetched tiles at several scales.
- [ ] Toggle imagery visibility repeatedly.
- [ ] Save, close, and reopen the map.
- [ ] Export or print a view containing the imagery.
- [ ] Uninstall the preview.
- [ ] Confirm official Mapper and its file associations still work after the
      preview uninstall.
- [ ] Record tester, Windows version, date, result, and any screenshots/logs.

## Trust and handoff

- [ ] Publish a stable prerelease/download URL rather than relying on an
      expiring Actions artifact link.
- [ ] Publish the generated release manifest beside the downloads.
- [ ] State explicitly whether the installer is signed. If unsigned, say that
      it is not an official upstream release and document the expected
      SmartScreen flow.
- [ ] State that only Windows x64 is supported for this preview.
- [ ] Include the imagery support boundary above in the user-facing note.
- [ ] Include a rollback/fallback path and a support contact.
- [ ] Link the final record to issues
      [#2](https://github.com/EthanOConnor/mapper/issues/2),
      [#5](https://github.com/EthanOConnor/mapper/issues/5), and
      [#8](https://github.com/EthanOConnor/mapper/issues/8).
- [ ] Record Mack's acceptance or class-blocking feedback in
      [#3](https://github.com/EthanOConnor/mapper/issues/3).

## Deliberately deferred until after Mack

- ArcGIS metadata discovery and arbitrary tile-matrix support.
- Provider-specific identification and visible attribution support.
- Atomic, non-overwriting generated imagery XML.
- Visible network error reporting.
- Bounded per-template tile memory.
- Upstream catch-up and conflict resolution.
- PR-triggered full packaging CI and immutable dependency hashes.

The pre-Mack branch remains based on post-PR #14. Upstream catch-up is deferred
because the missing upstream commits do not fix the tiled/TLS path and would
introduce unrelated conflict resolution plus a new full validation cycle.
