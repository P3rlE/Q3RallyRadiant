#!/bin/sh

# installs a game pack
# Usage:
#   install-gamepack.sh gamepack installdir

set -ex

: ${CP:=cp}
: ${CP_R:=cp -r}
: ${PACKFILTER:=}

pack=$1
dest=$2

# Some per-game workaround for malformed gamepack
case $pack in
	*/JediAcademyPack)
		pack="$pack/Tools"
	;;
	*/PreyPack|*/Quake3Pack)
		pack="$pack/tools"
	;;
	*/WolfPack)
		pack="$pack/bin"
	;;
	*/SmokinGunsPack|*/UnvanquishedPack)
		pack="$pack/build/netradiant"
	;;
	*/Q3RallyPack)
		pack="$pack/tools/gamepacks"
	;;
	*/WoPPack)
		pack="$pack/netradiant"
	;;
esac

for GAMEFILE in "$pack/games"/*.game; do
	if [ x"$GAMEFILE" != x"$pack/games/*.game" ]; then
		GAME=$(basename "$GAMEFILE" .game)
		case " $PACKFILTER " in
			"  ")
				;;
			*" $GAME "*)
				;;
			*)
				continue
				;;
		esac
		$CP "$GAMEFILE" "$dest/games/"
	fi
done
for GAMEDIR in "$pack"/*.game; do
	if [ x"$GAMEDIR" != x"$pack/*.game" ]; then
		GAME=$(basename "$GAMEDIR" .game)
		case " $PACKFILTER " in
			"  ")
				;;
			*" $GAME "*)
				;;
			*)
				continue
				;;
		esac
		$CP_R "$GAMEDIR" "$dest/"
	fi
done
