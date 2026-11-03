# RallyRoute: BPD files and track lengths

The `.bpd` files next to the `.map` (the `baseq3r/maps` folder that ships with
the game) are the reference race route. RallyRoute edits the checkpoint keys
`bezierPos`/`bezierDir` as a working copy and imports/exports them per track
length.

## File names

The plugin uses the same names the game loads (Q3Rally `docs/bpd-track-lengths.md`):

- `<map>_tl<n>.bpd` (n = 0 short, 1 medium, 2 long) when a `rally_checkpoint`
  is limited by `onlyTrackLength`, or when such files already exist.
- `<map>.bpd` otherwise; all lengths share it.

## Working on one track length

`Plugins > RallyRoute > Track Length: Short / Medium / Long` (or the combo box
in *Edit Selected Checkpoint...*) selects the length. Preview, checkpoint
navigation, the curve editor, Import, Export, Route Status and the Sprint
start/finish gates then only use entities whose `onlyTrackLength` contains it.

Switching the length:

1. If the Bezier keys of the current length differ from its BPD (or it has no
   BPD yet), RallyRoute asks to save them first (Yes / No / Cancel).
2. The new length's BPD is loaded into its checkpoints (one undo step).

Maps that share one `<map>.bpd` switch without asking.

## Opening a map

RallyRoute selects the first enabled length that has checkpoints. If its keys
differ from the BPD, it offers to load the BPD (the reference). For every
enabled length without a BPD it writes a seed from the best matching forward
Ghost (`track_length` from the Ghost header) next to the map; existing files
are never overwritten.

## Route Status

Shows the active length, enabled lengths, whether checkpoints differ between
lengths, checkpoint numbering (must be 1..N per length), anchor/handle counts,
the BPD path and whether it matches the map. A copy of the same file in
`AppData/Roaming/Q3Rally/baseq3r/maps` is reported, because it overrides the
shipped file in the game.
