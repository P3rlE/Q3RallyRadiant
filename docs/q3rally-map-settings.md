# Q3Rally worldspawn settings

## Using the editor

For the Q3Rally gamepack, File > New Map first opens worldspawn settings.
OK creates the new map; Cancel leaves the current map in place. The existing
unsaved-map confirmation still applies before starting a new map.
File > Map Settings opens the same dialog for an existing map. Accepted changes
form one undoable command; canceling or accepting unchanged values makes no edit.
Other gamepacks retain their usual New Map behavior.

Checked keys are written explicitly. Unchecked keys are omitted, preserving the
engine/compiler distinction between absent keys and explicit values. New maps
preselect runtime defaults; optional compiler values remain unset. The Additional
keys tab preserves and edits keys outside the predefined list. Classname is fixed.
Descriptions and defaults are available as tooltips. Existing values are not
normalized on opening or accepting the dialog. Newly edited runtime values are
validated; unchanged legacy values are retained.

## Sources and scope

Runtime defaults: Q3Rally g_spawn.c (including gravity 1400), g_weather.c,
cg_rally_fx.c (wetness), tr_bsp.c and tr_postprocess.c (haze format).
Compiler/terrain descriptions: q3rally.game/baseq3r/entities.def. Compiler keys
and aliases are documented gamepack options; availability depends on the
q3map2 build and compile settings. The schema is not a promise to expose every
possible q3map2 extension. Other keys remain available through Additional keys.

The predefined fields are in radiant/q3rally_mapsettings_fields.inc.

| Group | Key | Default shown | Input type |
|---|---|---|---|
| General | `message` | `Untitled` | text |
| General | `music` | `` | text |
| General | `scoreboardmusic` | `` | text |
| General | `gravity` | `1400` | positive |
| General | `reflectionImage` | `/textures/reflect/chrometest2` | text |
| Race and environment | `allowShortTrack` | `1` | bool |
| Race and environment | `allowMediumTrack` | `1` | bool |
| Race and environment | `allowLongTrack` | `1` | bool |
| Race and environment | `reversable` | `0` | bool |
| Race and environment | `enableDust` | `0` | bool |
| Race and environment | `enableSnow` | `0` | bool |
| Race and environment | `enableBreath` | `0` | bool |
| Weather and wind | `weather` | `` | weather |
| Weather and wind | `weatherChance` | `40` | chance |
| Weather and wind | `weatherCycle` | `90 300` | cycle |
| Weather and wind | `weatherStrength` | `0.4 1` | strength |
| Weather and wind | `weatherWind` | `30 250` | wind |
| Weather and wind | `weatherWindDir` | `-1` | direction |
| Lighting and look | `haze` | `` | text |
| Lighting and look | `_haze` | `` | text |
| Lighting and look | `colorlut` | `` | text |
| Lighting and look | `_colorlut` | `` | text |
| Lighting and look | `autoExposureMinMax` | `` | pair |
| Compiler and terrain | `_ambient` | `` | text |
| Compiler and terrain | `ambient` | `` | text |
| Compiler and terrain | `_color` | `` | text |
| Compiler and terrain | `gridsize` | `` | text |
| Compiler and terrain | `_blocksize` | `` | text |
| Compiler and terrain | `_minlight` | `` | text |
| Compiler and terrain | `_minvertexlight` | `` | text |
| Compiler and terrain | `_mingridlight` | `` | text |
| Compiler and terrain | `_keeplights` | `` | text |
| Compiler and terrain | `_noshadersun` | `` | text |
| Compiler and terrain | `_farplanedist` | `` | text |
| Compiler and terrain | `_foghull` | `` | text |
| Compiler and terrain | `_lightmapscale` | `` | text |
| Compiler and terrain | `_cs` | `` | text |
| Compiler and terrain | `_castshadows` | `` | text |
| Compiler and terrain | `_rs` | `` | text |
| Compiler and terrain | `_receiveshadows` | `` | text |
| Compiler and terrain | `_celshader` | `` | text |
| Compiler and terrain | `_indexmap` | `` | text |
| Compiler and terrain | `alphamap` | `` | text |
| Compiler and terrain | `_layers` | `` | text |
| Compiler and terrain | `layers` | `` | text |
| Compiler and terrain | `_shader` | `` | text |
| Compiler and terrain | `shader` | `` | text |
| Compiler and terrain | `remapshader` | `` | text |
| Compiler and terrain | `vertexremapshader` | `` | text |
| Weather and wind | `wetness` | `0` | unit |

## Regression tests

From a Qt5 development environment in the repository root:

```sh
g++ -std=c++20 -fPIC -Iinclude -Ilibs -Iradiant $(pkg-config --cflags Qt5Widgets) \
    regression_tests/mapsettings_test.cpp -o regression_tests/mapsettings_test \
    $(pkg-config --libs Qt5Widgets)
QT_QPA_PLATFORM=offscreen ./regression_tests/mapsettings_test
```

On Windows the output executable can use the .exe suffix. An optional first
argument saves a preview PNG. Tests cover numeric/range validation, Cancel,
new-map defaults, preservation of unknown and unchanged legacy values, and
editing an existing field. Qt tests and the Windows Radiant build pass.

Manual editor verification remains: New Map cancellation with unsaved geometry,
Map Settings on a real map, save/reopen, Undo/Redo, and a non-Q3Rally gamepack.
The Qt offscreen dialog preview has been visually inspected.
