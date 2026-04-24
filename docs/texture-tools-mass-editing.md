# Texture-Tools: Produktive Massenbearbeitung

Diese Änderung vereinheitlicht die Bedienlogik für Textur-Operationen auf **Faces und Patches** in der Surface-Inspector-UI.

## Gemeinsames Bedienkonzept

Die Oberfläche nutzt jetzt einen gemeinsamen Apply-Pfad (`SurfaceInspector_BatchApplySelection`), der:

- Face-/Brush-Texturpfade via `Select_SetTexdef` (BrushPrimit/Face-Texdef-Logik),
- Patch-Texturpfade via `Patch_SetTexdef`,
- und damit auch bestehende Textool-nahe Shift/Scale/Rotate-Workflows

unter einer einheitlichen Bedienung zusammenführt.

## Live-Vorschau und Undo/Redo

- Neue Option **Live Preview** im Surface Inspector.
- Ist die Option aktiv, werden Änderungen an Shift/Scale/Rotate beim Editieren direkt auf die Selektion angewandt.
- Jede Vorschau-Anwendung läuft über `UndoableCommand` und ist daher Undo/Redo-kompatibel.
- Zusätzlich gibt es **Batch Apply**, um die aktuellen Dialogwerte gesammelt auf die komplette Selektion anzuwenden.

## Presets

Neue Presets im Surface Inspector:

- **Road**
- **Terrain**
- **Decal**

`Apply Preset` setzt mehrere UV-Parameter in einem Schritt und führt anschließend Fit für die eingestellten Repeat-Werte aus.

## Batch-Apply auf Selektion

`Batch Apply` arbeitet auf der aktuellen Selektion und schließt damit Multi-Entity-/Multi-Brush-Fälle ein, sofern sie selektiert sind.

## Shortcut-Konflikte

Im Dialog ist ein Hinweis ergänzt:

- **FitTexture = Ctrl+F** kann mit Such-/Find-Shortcuts kollidieren (je nach fokussiertem Widget/Plattform).

