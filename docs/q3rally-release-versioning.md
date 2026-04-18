# Q3RallyRadiant Release Versioning

Dieses Repository nutzt für `RADIANT_VERSION_NUMBER` das Q3Rally-Schema:

- **Format:** `<major>.<minor>[.<patch>]-q3r`
- **Beispiel:** `2026.1-q3r`
- **Mit Patch:** `2026.1.1-q3r`

## Bedeutung der Versionsbestandteile

- **Major** (`major`, in der Regel Jahr):
  - Erhöhen bei größeren, sichtbaren Release-Zyklen (z. B. neues Release-Jahr oder größere Kompatibilitäts-/Workflow-Änderungen).
- **Minor** (`minor`):
  - Erhöhen bei regulären Feature-Releases innerhalb eines Major-Zyklus.
- **Patch** (`patch`, optional):
  - Erhöhen bei Bugfix-/Hotfix-Releases ohne neue größere Features.
  - Kann bei `0` weggelassen werden (`2026.1-q3r` statt `2026.1.0-q3r`).

## Ableitung der Build-Makros

Im `Makefile` werden daraus konsistent folgende Makros erzeugt:

- `RADIANT_VERSION` (vollständige Version für UI/Logs)
- `RADIANT_MAJOR_VERSION` (1. Segment)
- `RADIANT_MINOR_VERSION` (2. Segment)

Diese Makros werden über `CPPFLAGS_COMMON` als `-D...` in alle relevanten Binaries injiziert.

## `-git-<hash>` Anhängung

Wenn `git rev-parse --short HEAD` verfügbar ist, wird automatisch angehängt:

- `RADIANT_VERSION` → `...-git-<hash>`
- `Q3MAP_VERSION` → `...-git-<hash>`

Damit sind Entwicklungsbuilds in UI/Logs eindeutig einer Commit-Version zuordenbar.

## Artefakt-/Installer-Benennung

Release-Artefakte übernehmen dasselbe Schema direkt aus `RADIANT_VERSION_NUMBER`:

- Source-Release: `q3rallyradiant-<version>-<YYYYMMDD>.tar.bz2`
- Windows-Release: `q3rallyradiant-<version>-<YYYYMMDD>-win32-7z.exe`

Damit sind Versionstext in UI/Logs und Paketnamen konsistent.

## Installations-/Laufzeit-Validierung

Q3RallyRadiant prüft beim Start produkt-spezifische Markerdateien im Installationsverzeichnis:

- `Q3RALLY_RADIANT_MAJOR`
- `Q3RALLY_RADIANT_MINOR`

Diese müssen zu `RADIANT_MAJOR_VERSION` und `RADIANT_MINOR_VERSION` des laufenden Binaries passen.

Zusätzlich nutzt Q3RallyRadiant seit dem neuen Schema einen separaten Settings-Pfad:

- `Q3RallyRadiant-1.<major>.<minor>/` (neu, konfliktfrei neben NetRadiant)
- `1.<major>.<minor>/` (legacy)

Falls nur ein Legacy-Pfad vorhanden ist, kann dessen Inhalt beim ersten Start optional importiert werden. Danach bleiben NetRadiant und Q3RallyRadiant in getrennten Konfigurationsverzeichnissen.
